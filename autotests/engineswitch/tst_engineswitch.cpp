/*
 * Copyright 2026 Arktos Browser - IT-Space
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

#include "qtest_arktos.h"

#include <adblockmanager.h>
#include <adblockresourcehandler.h>
#include <browserapplication.h>
#include <browsermainwindow.h>
#include <browserpaths.h>
#include <containermanager.h>
#include <engineindicator.h>
#include <engineinterface.h>
#include <engineregistry.h>
#include <enginetab.h>
#include <lineedit.h>
#include <profilesettings.h>
#include <schemeaccesshandler.h>
#include <tabbar.h>
#include <tabwidget.h>
#include <toolbarsearch.h>
#include <webpage.h>
#include <webview.h>

#include "tlsfixture.h"

#include <qabstractbutton.h>
#include <qdir.h>
#include <qelapsedtimer.h>
#include <qfile.h>
#include <qlabel.h>
#include <qmenu.h>
#include <qmessagebox.h>
#include <qset.h>
#include <qsplitter.h>
#include <qstackedwidget.h>
#include <qtcpserver.h>
#include <qtcpsocket.h>
#include <qtemporarydir.h>
#include <qtimer.h>
#include <qurlquery.h>
#include <qwebenginepage.h>
#include <qsslsocket.h>

#ifdef ARKTOS_WEBKIT
#include "webkitbackend.h"
#endif
#ifdef ARKTOS_ADBLOCK_RUST
#include "adblockrustengine.h"
#endif

#include <qjsondocument.h>
#include <qjsonobject.h>

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

// Clicks the requested button on any modal QMessageBox that pops while
// this object is alive — the abp:subscribe flow prompts through
// QMessageBox::question/exec, which runs a real modal dialog even
// under the offscreen QPA (the schemehandlers lane's helper,
// repeated file-locally).
class ModalAnswer : public QObject
{
public:
    ModalAnswer(QMessageBox::StandardButton button, QObject *parent = nullptr)
        : QObject(parent)
        , m_button(button)
    {
        QTimer *timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this]() {
            QMessageBox *box = qobject_cast<QMessageBox *>(
                QApplication::activeModalWidget());
            if (!box)
                return;
            QAbstractButton *button = box->button(m_button);
            if (button)
                button->click();
            else
                box->reject();
            ++m_answered;
        });
        timer->start(50);
    }

    int answered() const { return m_answered; }

private:
    QMessageBox::StandardButton m_button;
    int m_answered = 0;
};

// WK19 — the WebKit cert-error leg needs a TLS peer the WPE network
// process will reject AND that can serve HTTP once an exception is
// installed: the shared TlsFixture's -www mode (`openssl s_server`
// holding the shared selfsigned scratch leaf — an in-process
// QSslServer was tried first, but the GnuTLS handshake inside the
// WebKit sandbox stalls against it, leaving the navigation to time
// out without any signal; a real OpenSSL peer answers the rejection
// in ~100 ms).
static QUrl tlsFixtureUrl(const TlsFixture &fixture)
{
    // 127.0.0.1 like every other WPE fixture — "localhost" is not
    // guaranteed to resolve inside the WebKit sandbox; the
    // CN=localhost leaf still fails validation (BAD_IDENTITY +
    // UNKNOWN_CA), which is exactly what the leg needs.
    return QUrl(QString::fromLatin1("https://127.0.0.1:%1/")
                .arg(fixture.port()));
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
    void engineTabChurn();
    void webkitBackendSurface();
    void webkitViewRenders();
    void webkitLoadSignals();
    void webkitHistory();
    void webkitProfileStorage();
    void webkitClearAndCookiePolicy();
    void webkitInput();
    void webkitZoomFindLifecycle();
    void webkitJsEval();
    void webkitScriptWorlds();
    void webkitPolicySchemes();
    void webkitContentBlocking();
    void webkitCertErrorInterstitial();
    void webkitContextMenu();
    void webkitPermissionRequest();
    void webkitDownload();
    void webkitGlPath();
    void webkitGlRasterFallback();
    void webkitRenderChurn();
    void engineBackendChurn();
    void servoViewChurn();
    void webkitGlSurvivesServoChurn();

private:
    FakeEngineBackend *m_fake = nullptr;
};

void tst_EngineSwitch::initTestCase()
{
    QCoreApplication::setApplicationName("tst_engineswitch");
    ProfileSettings settings;
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
    // EXT08's extension action strip sits between the indicator host
    // and the search cell on non-private windows.
    QCOMPARE(splitter->indexOf(search), 3);
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

// MEM11 — leak-audit churn leg: open + close an EngineTab several
// times on every live foreign backend.  Each cycle mints and tears
// down the backend's whole profile/page/view trio — webkit: WPEView
// + WebKitWebView + a WebsiteDataManager-bound WebkitProfile; servo:
// se_view_create/se_view_destroy on the shared engine (the ENG36
// churn path) — the exact lifecycle LSan hunts for orphans.  The
// QPointer guards are the in-test half of that check: a retained
// object fails HERE, a freed-object-with-stale-allocation fails in
// the leak report.
void tst_EngineSwitch::engineTabChurn()
{
    const QStringList candidates = { QStringLiteral("fake"),
                                     QStringLiteral("webkit"),
                                     QStringLiteral("servo") };
    int exercised = 0;
    for (const QString &id : candidates) {
        Engine::Backend *backend = EngineRegistry::backendForId(id);
        // Unlinked/unavailable backends are the hidden-not-dead
        // contract — skip them, never fail the leg on a missing
        // artifact.
        if (!backend || !backend->pageUnavailableReason().isEmpty())
            continue;
        ++exercised;
        TabWidget widget;
        for (int cycle = 0; cycle < 4; ++cycle) {
            widget.newTab();
            const int index = widget.count() - 1;
            QVERIFY2(widget.reloadTabInEngine(index, id),
                     qPrintable(id));
            EngineTab *foreign = widget.engineTab(index);
            QVERIFY2(foreign, qPrintable(id));
            QVERIFY2(foreign->page(), qPrintable(id));
            QPointer<EngineTab> tabGuard(foreign);
            QPointer<Engine::Page> pageGuard(foreign->page());
            QPointer<QWidget> viewGuard(foreign->page()->view());
            widget.closeTab(index);
            pumpDeletes();
            QVERIFY2(!tabGuard, qPrintable(id));
            QVERIFY2(!pageGuard, qPrintable(id));
            QVERIFY2(!viewGuard, qPrintable(id));
            QCOMPARE(widget.count(), 0);
        }
    }
    QVERIFY2(exercised >= 1, "no foreign backend available");
}

// WK07 — the WPE WebKit backend's registration contract: the
// static-init probe registers it only when the WPE packages were
// linked (servo's missing-artifact-hide, same shape), the display
// name carries the real version triple, and the capability table is
// the conservative spike-verified set with every named gap off.
void tst_EngineSwitch::webkitBackendSurface()
{
#ifdef ARKTOS_WEBKIT
    Engine::Backend *webkit =
        EngineRegistry::backendForId(QStringLiteral("webkit"));
#ifdef ARKTOS_WEBKIT_LINKED
    QVERIFY(webkit);
    QCOMPARE(webkit->id(), QStringLiteral("webkit"));

    // "WebKit x.y.z" — the user's explicit ask, the same shape as
    // "Servo 0.7.0": parse the triple out of the name rather than
    // trusting a label.
    const QStringList parts =
        webkit->displayName().split(QLatin1Char(' '));
    QCOMPARE(parts.count(), 2);
    QCOMPARE(parts.at(0), QStringLiteral("WebKit"));
    const QStringList triple = parts.at(1).split(QLatin1Char('.'));
    QCOMPARE(triple.count(), 3);
    for (const QString &field : triple) {
        bool ok = false;
        field.toInt(&ok);
        QVERIFY2(ok, qPrintable(webkit->displayName()));
    }

    // .devin/WEBKIT-SPIKE.md's conservative table — on: interception
    // (decide-policy + content-blocker), injection, eval, per-page
    // settings, lifecycle discard, context menus, cert override,
    // downloads; off: the named gaps (no per-attempt CookieAttempt
    // functor, no embedder devtools channel, unprobed
    // WebKitWebExtension).
    const Engine::Capabilities caps = webkit->capabilities();
    QVERIFY(caps.requestInterception);
    QVERIFY(caps.scriptInjection);
    QVERIFY(caps.scriptEvaluation);
    QVERIFY(!caps.cookieFilter);
    QVERIFY(caps.perPageSettings);
    QVERIFY(caps.lifecycleDiscard);
    QVERIFY(caps.contextMenuInfo);
    QVERIFY(caps.certificateOverride);
    QVERIFY(!caps.devTools);
    QVERIFY(!caps.extensions);
    QVERIFY(caps.downloads);

    // WK08: the backend hosts real (if minimal) pages now — the
    // switcher offers a live entry and no disabled-reason remains.
    QVERIFY(!webkit->engineNote().isEmpty());
    QVERIFY2(webkit->pageUnavailableReason().isEmpty(),
             qPrintable(webkit->pageUnavailableReason()));
#else
    // Compile-but-inert (webkit=1 without the WPE packages): the
    // probe's initialize() refused, so the backend never registered —
    // the same hidden-not-dead-entry contract as a missing servo
    // artifact.
    QVERIFY(!webkit);
#endif
#else
    QSKIP("webkit backend not in this build");
#endif
}

// WK08 — the acceptance leg: an EngineTab on the webkit backend (the
// host is UNCHANGED — createPage + createView is all it calls) loads
// a file:// fixture, loadFinished(true) arrives, and the raster path
// puts real non-blank pixels into the view widget.
void tst_EngineSwitch::webkitViewRenders()
{
#ifdef ARKTOS_WEBKIT_LINKED
    Engine::Backend *webkit =
        EngineRegistry::backendForId(QStringLiteral("webkit"));
    QVERIFY(webkit);
    QVERIFY2(webkit->pageUnavailableReason().isEmpty(),
             qPrintable(webkit->pageUnavailableReason()));

    Engine::ProfileOptions options;
    options.offTheRecord = true;   // ephemeral session — no disk state
    Engine::Profile *profile = webkit->createProfile(options, this);
    QVERIFY(profile);

    EngineTab tab(webkit, profile);
    tab.resize(800, 600);
    tab.show();
    QVERIFY(tab.page());
    auto *view = qobject_cast<WebkitView *>(tab.page()->view());
    QVERIFY(view);

    const QString fixturePath = QDir::temp().filePath(
        QStringLiteral("arktos-wk08-fixture.html"));
    {
        QFile fixture(fixturePath);
        QVERIFY(fixture.open(QIODevice::WriteOnly));
        fixture.write("<html><head><title>Wk08Render</title></head>"
                      "<body style='margin:0;background:#eef4ff'>"
                      "<div style='width:400px;height:300px;"
                      "background:#123a6f;color:#22aa77'>"
                      "rendered by WPE WebKit</div>"
                      "</body></html>");
    }

    QSignalSpy finishedSpy(tab.page(), &Engine::Page::loadFinished);
    tab.load(QUrl::fromLocalFile(fixturePath));
    QTRY_VERIFY2_WITH_TIMEOUT(finishedSpy.count() >= 1,
                              "loadFinished never arrived", 60000);
    QVERIFY2(finishedSpy.first().first().toBool(),
             "fixture load reported failure");
    QVERIFY2(tab.url() == QUrl::fromLocalFile(fixturePath),
             qPrintable(tab.url().toString()));
    QTRY_COMPARE_EQ(tab.title(), QStringLiteral("Wk08Render"));

    // Raster proof: at least one buffer imported, then keep pumping —
    // the first WPE buffer can be the blank pre-paint frame, so grab
    // until the fixture's witness colors land in the painted surface.
    QTRY_VERIFY_WITH_TIMEOUT(view->frameCount() >= 1, 30000);
    int distinctColors = 0;
    auto colorsReady = [&]() -> bool {
        const QImage shot = view->grab().toImage()
                .convertToFormat(QImage::Format_ARGB32);
        if (shot.isNull())
            return false;
        QSet<QRgb> colors;
        for (int y = 0; y < shot.height(); ++y) {
            const QRgb *row =
                reinterpret_cast<const QRgb *>(shot.constScanLine(y));
            for (int x = 0; x < shot.width(); ++x)
                colors.insert(row[x] | 0xff000000u);
        }
        distinctColors = colors.count();
        return colors.contains(QRgb(0xff123a6f))
            && colors.contains(QRgb(0xffeef4ff))
            && colors.count() >= 3;
    };
    QTRY_VERIFY2_WITH_TIMEOUT(colorsReady(),
             qPrintable(QStringLiteral("fixture colors never painted — "
                                       "%1 distinct colors")
                            .arg(distinctColors)), 30000);
#else
    QSKIP("webkit backend not linked in this build");
#endif
}

// WK09 — the Page load-signal contract on the webkit backend:
// started→progress→finished ordering, url/title/icon arrival,
// NavigationType off decide-policy, and stop()/reload() driving real
// load transitions.  WebKit only loads icons whose URL is http(s),
// so the fixture is a local HTTP server rather than a file:// page.
void tst_EngineSwitch::webkitLoadSignals()
{
#ifdef ARKTOS_WEBKIT_LINKED
    Engine::Backend *webkit =
        EngineRegistry::backendForId(QStringLiteral("webkit"));
    QVERIFY(webkit);
    QVERIFY2(webkit->pageUnavailableReason().isEmpty(),
             qPrintable(webkit->pageUnavailableReason()));

    // Fixture: a page that names a PNG favicon, and the icon itself.
    QByteArray faviconPng;
    {
        QImage icon(16, 16, QImage::Format_ARGB32);
        icon.fill(QColor(0x1a, 0x7f, 0x37));
        QBuffer buffer(&faviconPng);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(icon.save(&buffer, "PNG"));
    }
    const QByteArray pageBody =
        "<html><head><title>Wk09Signals</title>"
        "<link rel=\"icon\" type=\"image/png\" href=\"/favicon.png\">"
        "</head><body>wk09 signal fixture</body></html>";

    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    auto pending =
        QSharedPointer<QHash<QTcpSocket *, QByteArray>>::create();
    connect(&server, &QTcpServer::newConnection, &server,
            [&server, pageBody, faviconPng, pending]() {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            socket->setParent(&server);
            connect(socket, &QTcpSocket::readyRead, socket,
                    [socket, pageBody, faviconPng, pending]() {
                QByteArray &request = (*pending)[socket];
                request += socket->readAll();
                if (!request.contains("\r\n\r\n"))
                    return;
                QByteArray body = pageBody;
                QByteArray mime = "text/html";
                if (request.startsWith("GET /favicon.png")) {
                    body = faviconPng;
                    mime = "image/png";
                }
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: " + mime
                              + "\r\nContent-Length: "
                              + QByteArray::number(body.size())
                              + "\r\nConnection: close\r\n\r\n" + body);
                socket->disconnect();   // one request per connection
                socket->disconnectFromHost();
                pending->remove(socket);
            });
        }
    });
    const QUrl url(QStringLiteral("http://127.0.0.1:%1/")
                       .arg(server.serverPort()));

    Engine::ProfileOptions options;
    options.offTheRecord = true;   // ephemeral session — no disk state
    Engine::Profile *profile = webkit->createProfile(options, this);
    QVERIFY(profile);

    EngineTab tab(webkit, profile);
    tab.resize(800, 600);
    tab.show();
    QVERIFY(tab.page());
    auto *page = qobject_cast<WebkitPage *>(tab.page());
    QVERIFY(page);

    QStringList order;   // signal arrival order, tags only
    QList<int> progressValues;
    QList<QUrl> urls;
    QStringList titles;
    QList<QIcon> icons;
    QList<bool> finishedOk;
    connect(page, &Engine::Page::loadStarted,
            page, [&]() { order << QStringLiteral("started"); });
    connect(page, &Engine::Page::loadProgress,
            page, [&](int p) {
        order << QStringLiteral("progress");
        progressValues << p;
    });
    connect(page, &Engine::Page::loadFinished,
            page, [&](bool ok) {
        order << QStringLiteral("finished");
        finishedOk << ok;
    });
    connect(page, &Engine::Page::urlChanged,
            page, [&](const QUrl &u) {
        order << QStringLiteral("url");
        urls << u;
    });
    connect(page, &Engine::Page::titleChanged,
            page, [&](const QString &t) {
        order << QStringLiteral("title");
        titles << t;
    });
    connect(page, &Engine::Page::iconChanged,
            page, [&](const QIcon &i) {
        order << QStringLiteral("icon");
        icons << i;
    });

    tab.load(url);
    QTRY_VERIFY2_WITH_TIMEOUT(!finishedOk.isEmpty(),
                              "loadFinished never arrived", 60000);
    QVERIFY2(finishedOk.first(), "fixture load reported failure");

    // Order contract: exactly one started→finished pair per load,
    // progress strictly inside it.  uri/title/icon notifies are
    // property-driven and can land anywhere in the window (WebKit
    // moves uri at request time and the title can resolve a tick
    // after FINISHED) — only the load triple is ordered.
    QCOMPARE(order.count(QStringLiteral("started")), 1);
    QCOMPARE(order.count(QStringLiteral("finished")), 1);
    QVERIFY(order.contains(QStringLiteral("progress")));
    const int startedAt = order.indexOf(QStringLiteral("started"));
    const int finishedAt = order.indexOf(QStringLiteral("finished"));
    QVERIFY2(startedAt >= 0 && startedAt < finishedAt,
             qPrintable(order.join(QLatin1Char(','))));
    for (int i = startedAt + 1; i < finishedAt; ++i)
        QVERIFY2(order.at(i) != QStringLiteral("started"),
                 qPrintable(order.join(QLatin1Char(','))));
    QVERIFY(!progressValues.isEmpty());
    QCOMPARE(progressValues.last(), 100);

    QVERIFY(urls.contains(url));
    // Title and icon notifies resolve a tick after FINISHED.
    QTRY_VERIFY2_WITH_TIMEOUT(
            titles.contains(QStringLiteral("Wk09Signals")),
            "titleChanged never arrived", 30000);

    // The icon fetch is async past loadFinished — the property lands
    // once the favicon database answer comes back.
    QTRY_VERIFY2_WITH_TIMEOUT(!icons.isEmpty() && !icons.last().isNull(),
                              "iconChanged never arrived", 30000);
    const QImage iconImage = icons.last().pixmap(64, 64).toImage()
        .convertToFormat(QImage::Format_ARGB32);
    const QColor pixel = QColor::fromRgba(iconImage.pixel(8, 8));
    QVERIFY2(pixel.green() > 100 && pixel.red() < 60,
             qPrintable(QStringLiteral("favicon color %1")
                            .arg(pixel.name())));

    // decide-policy maps embedder-initiated load() to Other (WebKit
    // has no Typed).
    QCOMPARE(page->lastNavigationType(), Engine::NavigationType::Other);

    // reload() drives a real second cycle.
    order.clear();
    finishedOk.clear();
    page->reload();
    QTRY_VERIFY2_WITH_TIMEOUT(!finishedOk.isEmpty(),
                              "reload never finished", 60000);
    QVERIFY2(finishedOk.first(), "reload reported failure");
    QCOMPARE(order.count(QStringLiteral("started")), 1);
    QCOMPARE(order.count(QStringLiteral("finished")), 1);
    QVERIFY2(order.indexOf(QStringLiteral("started"))
                 < order.indexOf(QStringLiteral("finished")),
             qPrintable(order.join(QLatin1Char(','))));
    QCOMPARE(page->lastNavigationType(), Engine::NavigationType::Reload);

    // stop() mid-load answers loadFinished(false) — a server that
    // accepts and never answers keeps the load in flight.
    QTcpServer staller;
    QVERIFY(staller.listen(QHostAddress::LocalHost));
    connect(&staller, &QTcpServer::newConnection, &staller,
            [&staller]() {
        staller.nextPendingConnection()->setParent(&staller);
    });
    order.clear();
    finishedOk.clear();
    page->load(QUrl(QStringLiteral("http://127.0.0.1:%1/hang")
                        .arg(staller.serverPort())));
    QTRY_VERIFY2_WITH_TIMEOUT(order.contains(QStringLiteral("started")),
                              "stalled load never started", 30000);
    page->stop();
    QTRY_VERIFY2_WITH_TIMEOUT(!finishedOk.isEmpty(),
                              "stop() produced no loadFinished", 30000);
    QVERIFY2(!finishedOk.first(), "stopped load reported success");
    QVERIFY(!page->isLoading());
    QCOMPARE(order.last(), QStringLiteral("finished"));
#else
    QSKIP("webkit backend not linked in this build");
#endif
}

// WK10 — the real WebKitBackForwardList behind the Page history
// contract: a three-page session exposes the oldest-first linear
// stack with the current index, back/forward slices honor the
// maxItems cap, back()/forward() and goToHistoryEntry() drive real
// bf navigations, and a fresh load after going back truncates the
// forward list.  The fixture is a local HTTP server answering
// Cache-Control: no-store — a cacheable page is bf-restored from
// WebKit's page cache without a navigation decision, and the
// BackOrForward type assert below needs decide-policy to fire.
// Titles are asserted with a pump because WebKit commits them a
// tick late.
void tst_EngineSwitch::webkitHistory()
{
#ifdef ARKTOS_WEBKIT_LINKED
    Engine::Backend *webkit =
        EngineRegistry::backendForId(QStringLiteral("webkit"));
    QVERIFY(webkit);
    QVERIFY2(webkit->pageUnavailableReason().isEmpty(),
             qPrintable(webkit->pageUnavailableReason()));

    Engine::ProfileOptions options;
    options.offTheRecord = true;   // ephemeral session — no disk state
    Engine::Profile *profile = webkit->createProfile(options, this);
    QVERIFY(profile);

    EngineTab tab(webkit, profile);
    tab.resize(800, 600);
    tab.show();
    QVERIFY(tab.page());
    auto *page = qobject_cast<WebkitPage *>(tab.page());
    QVERIFY(page);

    // Fixture: /a /b /c /d, each a titled page that must never be
    // page-cache eligible.
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    auto pending =
        QSharedPointer<QHash<QTcpSocket *, QByteArray>>::create();
    connect(&server, &QTcpServer::newConnection, &server,
            [&server, pending]() {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            socket->setParent(&server);
            connect(socket, &QTcpSocket::readyRead, socket,
                    [socket, pending]() {
                QByteArray &request = (*pending)[socket];
                request += socket->readAll();
                if (!request.contains("\r\n\r\n"))
                    return;
                const QByteArray path =
                    request.split(' ').value(1, "/");
                const QByteArray tag = path.mid(1).toUpper();
                const QByteArray body =
                    "<html><head><title>Wk10" + tag
                    + "</title></head><body>Wk10" + tag
                    + "</body></html>";
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html"
                              "\r\nCache-Control: no-store"
                              "\r\nContent-Length: "
                              + QByteArray::number(body.size())
                              + "\r\nConnection: close\r\n\r\n" + body);
                socket->disconnect();
                socket->disconnectFromHost();
                pending->remove(socket);
            });
        }
    });
    const QUrl urlA(QStringLiteral("http://127.0.0.1:%1/a")
                        .arg(server.serverPort()));
    const QUrl urlB(QStringLiteral("http://127.0.0.1:%1/b")
                        .arg(server.serverPort()));
    const QUrl urlC(QStringLiteral("http://127.0.0.1:%1/c")
                        .arg(server.serverPort()));
    const QUrl urlD(QStringLiteral("http://127.0.0.1:%1/d")
                        .arg(server.serverPort()));

    auto loadAndWait = [&page](const QUrl &u) -> bool {
        QSignalSpy spy(page, &Engine::Page::loadFinished);
        page->load(u);
        return spy.wait(60000) && spy.first().first().toBool();
    };
    // bf hops are real loads — wait for their own loadFinished
    // before issuing the next one (a fresh load mid-flight cancels
    // the in-flight navigation and its loadFinished(false) would
    // trip the spy).
    auto bfNav = [&page](const std::function<void()> &go,
                         const QUrl &u) -> bool {
        QSignalSpy spy(page, &Engine::Page::loadFinished);
        go();
        return spy.wait(60000) && spy.first().first().toBool()
            && page->url() == u;
    };

    QVERIFY2(loadAndWait(urlA), "fixture A load failed");
    QCOMPARE(page->historyCount(), 1);
    QCOMPARE(page->currentHistoryIndex(), 0);
    QVERIFY(!page->canGoBack());
    QVERIFY(!page->canGoForward());
    QCOMPARE(page->historyItems().count(), 1);
    QCOMPARE(page->historyItems().first().url, urlA);

    QVERIFY2(loadAndWait(urlB), "fixture B load failed");
    QVERIFY2(loadAndWait(urlC), "fixture C load failed");
    QTRY_VERIFY2_WITH_TIMEOUT(
        page->historyCount() == 3 && page->currentHistoryIndex() == 2,
        "three-load stack never formed", 30000);
    QVERIFY(page->canGoBack());
    QVERIFY(!page->canGoForward());

    // Stack shape: oldest first, index = stack position.
    const QList<Engine::HistoryEntry> items = page->historyItems();
    QCOMPARE(items.count(), 3);
    QCOMPARE(items.at(0).url, urlA);
    QCOMPARE(items.at(1).url, urlB);
    QCOMPARE(items.at(2).url, urlC);
    QCOMPARE(items.at(0).index, 0);
    QCOMPARE(items.at(1).index, 1);
    QCOMPARE(items.at(2).index, 2);

    const QList<Engine::HistoryEntry> back = page->backItems(10);
    QCOMPARE(back.count(), 2);
    QCOMPARE(back.at(0).url, urlA);
    QCOMPARE(back.at(1).url, urlB);      // nearest current is last
    QCOMPARE(page->backItems(1).count(), 1);
    QCOMPARE(page->backItems(1).first().url, urlB);
    QVERIFY(page->forwardItems(10).isEmpty());

    // Item titles land once the documents have committed them.
    QTRY_VERIFY2_WITH_TIMEOUT(
        page->historyItems().at(0).title == QStringLiteral("Wk10A")
        && page->historyItems().at(1).title == QStringLiteral("Wk10B")
        && page->historyItems().at(2).title == QStringLiteral("Wk10C"),
        "back-forward item titles never settled", 30000);

    // back() is a real navigation — the bf entry point stamps the
    // BackOrForward type (WebKit emits no decide-policy for bf hops)
    // and the forward slice gains C.  The index assert pumps: the
    // uri commits a tick before the BF list's current item moves.
    QVERIFY2(bfNav([&page] { page->back(); }, urlB),
             "back() to B failed");
    QCOMPARE(page->lastNavigationType(),
             Engine::NavigationType::BackOrForward);
    QTRY_COMPARE_EQ(page->currentHistoryIndex(), 1);
    QCOMPARE(page->historyCount(), 3);
    QVERIFY(page->canGoBack());
    QVERIFY(page->canGoForward());
    QCOMPARE(page->backItems(10).count(), 1);
    QCOMPARE(page->backItems(10).first().url, urlA);
    QCOMPARE(page->forwardItems(10).count(), 1);
    QCOMPARE(page->forwardItems(10).first().url, urlC);

    // goToHistoryEntry reaches any stack index — jump back two.
    QVERIFY2(bfNav([&page] {
                 page->goToHistoryEntry(page->historyItems().first());
             }, urlA),
             "goToHistoryEntry to A failed");
    QTRY_COMPARE_EQ(page->currentHistoryIndex(), 0);
    QVERIFY(!page->canGoBack());
    QVERIFY(page->canGoForward());
    QCOMPARE(page->forwardItems(10).count(), 2);
    QCOMPARE(page->forwardItems(10).at(0).url, urlB);
    QCOMPARE(page->forwardItems(10).at(1).url, urlC);

    // forward() walks the slice back toward C.
    QVERIFY2(bfNav([&page] { page->forward(); }, urlB),
             "forward() to B failed");
    QTRY_COMPARE_EQ(page->currentHistoryIndex(), 1);

    // A fresh load after going back truncates the forward list —
    // WebKit prunes it when the new navigation commits.
    QVERIFY2(bfNav([&page] { page->back(); }, urlA),
             "back() to A failed");
    QVERIFY2(loadAndWait(urlD), "fixture D load failed");
    QTRY_VERIFY2_WITH_TIMEOUT(
        page->historyCount() == 2 && page->currentHistoryIndex() == 1,
        "forward list never pruned", 30000);
    QVERIFY(page->forwardItems(10).isEmpty());
    QCOMPARE(page->historyItems().at(1).url, urlD);
    QVERIFY(!page->canGoForward());
#else
    QSKIP("webkit backend not linked in this build");
#endif
}

// WK11 — per-profile storage: a profile carrying a storage identity
// gets a private WebKitNetworkSession/WebKitWebsiteDataManager rooted
// inside the profile's storage tree (persona names land at
// profiles/<id>/WebKit — the VSEC02 sibling of QtWebEngine's dir —
// with cache at profiles/<id>/cache/WebKit; an explicit dataPath gets
// a collision-proof <dataPath>/WebKit leaf), storage written on one
// profile is invisible to another, and an OTR profile's ephemeral
// session writes nothing to disk.
void tst_EngineSwitch::webkitProfileStorage()
{
#ifdef ARKTOS_WEBKIT_LINKED
    Engine::Backend *webkit =
        EngineRegistry::backendForId(QStringLiteral("webkit"));
    QVERIFY(webkit);
    QVERIFY2(webkit->pageUnavailableReason().isEmpty(),
             qPrintable(webkit->pageUnavailableReason()));

    // Storage APIs need a real (non-file) origin — the fixture is a
    // one-page local HTTP server.
    const QByteArray pageBody =
        "<html><head><title>Wk11Storage</title></head>"
        "<body>wk11</body></html>";
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    connect(&server, &QTcpServer::newConnection, &server,
            [&server, pageBody]() {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            socket->setParent(&server);
            connect(socket, &QTcpSocket::readyRead, socket,
                    [socket, pageBody]() {
                if (!socket->readAll().contains("\r\n\r\n"))
                    return;
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html"
                              "\r\nContent-Length: "
                              + QByteArray::number(pageBody.size())
                              + "\r\nConnection: close\r\n\r\n" + pageBody);
                socket->disconnect();
                socket->disconnectFromHost();
            });
        }
    });
    const QUrl url(QStringLiteral("http://127.0.0.1:%1/")
                       .arg(server.serverPort()));

    auto loadSync = [](Engine::Page *p, const QUrl &u) {
        QSignalSpy spy(p, &Engine::Page::loadFinished);
        p->load(u);
        return spy.wait(60000) && spy.first().first().toBool();
    };
    // runJavaScript's completion arrives through the event loop —
    // poll it (QTRY_* macros return early, which a non-void lambda
    // cannot do).
    struct EvalOut { QVariant value; bool done = false; };
    auto evalSync = [](Engine::Page *p, const QString &src) -> EvalOut {
        EvalOut out;
        p->runJavaScript(src, [&](const QVariant &v) {
            out.value = v;
            out.done = true;
        });
        QElapsedTimer timer;
        timer.start();
        while (!out.done && timer.elapsed() < 30000)
            QTest::qWait(20);
        return out;
    };

    // --- persona-named profile A: profiles/<id>/WebKit ---------
    Engine::ProfileOptions optA;
    optA.storageName = QStringLiteral("arktos-wk11a");
    auto *profileA = qobject_cast<WebkitProfile *>(
        webkit->createProfile(optA, this));
    QVERIFY(profileA);
    QCOMPARE(profileA->storageName(), QStringLiteral("arktos-wk11a"));
    QVERIFY(!profileA->isOffTheRecord());
    // Laziness: no session (and no dirs) until the first page.
    QVERIFY(profileA->sessionDataDir().isEmpty());

    Engine::Page *pageA = webkit->createPage(profileA, this);
    QVERIFY2(pageA, "createPage refused a named profile");

    const QString dataDirA =
        BrowserPaths::profileDataDir(QStringLiteral("wk11a"))
        + QLatin1String("/WebKit");
    const QString cacheDirA =
        BrowserPaths::profileDataDir(QStringLiteral("wk11a"))
        + QLatin1String("/cache/WebKit");
    QCOMPARE(profileA->sessionDataDir(), dataDirA);
    QCOMPARE(profileA->sessionCacheDir(), cacheDirA);
    QVERIFY(!profileA->sessionIsEphemeral());
    QVERIFY(QDir(dataDirA).exists());
    QVERIFY(QDir(cacheDirA).exists());

    QVERIFY2(loadSync(pageA, url), "profile A fixture load failed");
    EvalOut write = evalSync(pageA, QStringLiteral(
        "(function(){localStorage.setItem('wk11','alpha');"
        "document.cookie='wk11=alpha;path=/';"
        "return 'done';})()"));
    QVERIFY2(write.done, "profile A storage write eval timed out");
    QCOMPARE(write.value.toString(), QStringLiteral("done"));
    QCOMPARE(evalSync(pageA, QStringLiteral(
                 "(function(){return localStorage.getItem('wk11')||'';})()"))
                 .value.toString(),
             QStringLiteral("alpha"));

    // --- persona B: same origin, disjoint store -----------------
    Engine::ProfileOptions optB;
    optB.storageName = QStringLiteral("arktos-wk11b");
    auto *profileB = qobject_cast<WebkitProfile *>(
        webkit->createProfile(optB, this));
    QVERIFY(profileB);
    Engine::Page *pageB = webkit->createPage(profileB, this);
    QVERIFY2(pageB, "createPage refused profile B");
    QCOMPARE(profileB->sessionDataDir(),
             BrowserPaths::profileDataDir(QStringLiteral("wk11b"))
                 + QLatin1String("/WebKit"));
    QVERIFY(profileB->sessionDataDir() != profileA->sessionDataDir());

    QVERIFY2(loadSync(pageB, url), "profile B fixture load failed");
    QCOMPARE(evalSync(pageB, QStringLiteral(
                 "(function(){return localStorage.getItem('wk11')||'';})()"))
                 .value.toString(),
             QStringLiteral(""));
    QVERIFY2(!evalSync(pageB, QStringLiteral(
                 "(function(){return document.cookie;})()"))
                 .value.toString().contains(QStringLiteral("wk11=alpha")),
             "profile B can see profile A's cookie");

    // --- explicit dataPath: <root>/WebKit leaf ------------------
    QTemporaryDir rootC;
    QTemporaryDir cacheC;
    QVERIFY(rootC.isValid() && cacheC.isValid());
    Engine::ProfileOptions optC;
    optC.storageName = QStringLiteral("arktos-container-x-y");
    optC.dataPath = rootC.path();
    optC.cachePath = cacheC.path();
    auto *profileC = qobject_cast<WebkitProfile *>(
        webkit->createProfile(optC, this));
    QVERIFY(profileC);
    Engine::Page *pageC = webkit->createPage(profileC, this);
    QVERIFY2(pageC, "createPage refused the path-rooted profile");
    QCOMPARE(profileC->sessionDataDir(),
             rootC.path() + QLatin1String("/WebKit"));
    QCOMPARE(profileC->sessionCacheDir(),
             cacheC.path() + QLatin1String("/WebKit"));

    QVERIFY2(loadSync(pageC, url), "profile C fixture load failed");
    QCOMPARE(evalSync(pageC, QStringLiteral(
                 "(function(){return localStorage.getItem('wk11')||'';})()"))
                 .value.toString(),
             QStringLiteral(""));
    // The live session still holds handles under the temp roots —
    // release it before the QTemporaryDirs unlink the trees.
    delete pageC;
    delete profileC;

    // --- hostile foreign name: slug leaf, no escape -------------
    Engine::ProfileOptions optD;
    optD.storageName = QStringLiteral("../../etc/passwd");
    auto *profileD = qobject_cast<WebkitProfile *>(
        webkit->createProfile(optD, this));
    QVERIFY(profileD);
    Engine::Page *pageD = webkit->createPage(profileD, this);
    QVERIFY2(pageD, "createPage refused the hostile-name profile");
    QVERIFY(profileD->sessionDataDir().startsWith(
        BrowserPaths::profileDataDir(BrowserPaths::activeProfileId())
        + QLatin1String("/WebKit/")));
    QVERIFY(!profileD->sessionDataDir().contains(QStringLiteral("..")));
    delete pageD;
    delete profileD;

    // --- artifacts land under the profile dir -------------------
    // WebKit flushes stores async — poll for the first real file
    // under A's tree (localstorage db, cookies.sqlite, ...).
    QTRY_VERIFY2_WITH_TIMEOUT(
            !QDir(dataDirA).entryInfoList(
                QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot
                | QDir::Hidden, QDir::NoSort).isEmpty(),
            "nothing was ever written under profile A's data dir",
            30000);

    // --- OTR: ephemeral session, zero files ---------------------
    Engine::ProfileOptions optOtr;
    optOtr.offTheRecord = true;
    // A hostile name on an OTR profile must not mint a store.
    optOtr.storageName = QStringLiteral("arktos-wk11otr");
    auto *profileOtr = qobject_cast<WebkitProfile *>(
        webkit->createProfile(optOtr, this));
    QVERIFY(profileOtr);
    QVERIFY(profileOtr->isOffTheRecord());
    Engine::Page *pageOtr = webkit->createPage(profileOtr, this);
    QVERIFY2(pageOtr, "createPage refused the OTR profile");
    QVERIFY(profileOtr->sessionIsEphemeral());
    QVERIFY(profileOtr->sessionDataDir().isEmpty());
    QVERIFY(profileOtr->sessionCacheDir().isEmpty());

    QVERIFY2(loadSync(pageOtr, url), "OTR fixture load failed");
    // In-memory storage still works — it just never persists.
    QCOMPARE(evalSync(pageOtr, QStringLiteral(
                 "(function(){try{localStorage.setItem('wk11','otr');"
                 "return localStorage.getItem('wk11')||'';}"
                 "catch(e){return 'denied';}})()")).value.toString(),
             QStringLiteral("otr"));
    // Nothing was ever minted for the OTR name — compute the path
    // WITHOUT profileDataDir() (which mkpaths as a side effect).
    QVERIFY(!QFileInfo::exists(
        BrowserPaths::dataRoot()
        + QLatin1String("/profiles/wk11otr")));
#else
    QSKIP("webkit backend not linked in this build");
#endif
}

// WK12 — clear(StorageAreas) -> webkit_website_data_manager_clear on
// the profile's session (cookie-manager read-back proves the wipe,
// a second page proves DOM storage dropped) and the cookie
// accept-policy -> WebKitCookieManager mapping: the persona's
// persisted cookies/* settings drive set_accept_policy at session
// materialization, and set_persistent_storage gives a named session
// a cookies.sqlite unless the keep policy is KeepUntilExit.
void tst_EngineSwitch::webkitClearAndCookiePolicy()
{
#ifdef ARKTOS_WEBKIT_LINKED
    Engine::Backend *webkit =
        EngineRegistry::backendForId(QStringLiteral("webkit"));
    QVERIFY(webkit);
    QVERIFY2(webkit->pageUnavailableReason().isEmpty(),
             qPrintable(webkit->pageUnavailableReason()));

    // Cookies + DOM storage need a real origin — a one-page local
    // HTTP fixture (the WK11 shape).
    const QByteArray pageBody =
        "<html><head><title>Wk12Clear</title></head>"
        "<body>wk12</body></html>";
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    connect(&server, &QTcpServer::newConnection, &server,
            [&server, pageBody]() {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            socket->setParent(&server);
            connect(socket, &QTcpSocket::readyRead, socket,
                    [socket, pageBody]() {
                if (!socket->readAll().contains("\r\n\r\n"))
                    return;
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html"
                              "\r\nContent-Length: "
                              + QByteArray::number(pageBody.size())
                              + "\r\nConnection: close\r\n\r\n" + pageBody);
                socket->disconnect();
                socket->disconnectFromHost();
            });
        }
    });
    const QUrl url(QStringLiteral("http://127.0.0.1:%1/")
                       .arg(server.serverPort()));

    auto loadSync = [](Engine::Page *p, const QUrl &u) {
        QSignalSpy spy(p, &Engine::Page::loadFinished);
        p->load(u);
        return spy.wait(60000) && spy.first().first().toBool();
    };
    struct EvalOut { QVariant value; bool done = false; };
    auto evalSync = [](Engine::Page *p, const QString &src) -> EvalOut {
        EvalOut out;
        p->runJavaScript(src, [&](const QVariant &v) {
            out.value = v;
            out.done = true;
        });
        QElapsedTimer timer;
        timer.start();
        while (!out.done && timer.elapsed() < 30000)
            QTest::qWait(20);
        return out;
    };
    // The cookie-manager read-back — WebKitCookieManager's
    // get_all_cookies through the WK12 diagnostic surface.
    auto cookiesSync = [](WebkitProfile *profile) -> QStringList {
        QStringList out;
        bool done = false;
        profile->fetchCookies(
            [&](const QStringList &cookies) {
                out = cookies;
                done = true;
            });
        QElapsedTimer timer;
        timer.start();
        while (!done && timer.elapsed() < 10000)
            QTest::qWait(20);
        return out;
    };

    // --- Leg 1: persistent persona — clear(CookiesArea) empties the
    // store, clear(AllAreas) drops DOM storage.  AcceptAlways with
    // no third-party block maps to ACCEPT_ALWAYS (WPE ordinal 0) and
    // KeepUntilExpire gets the cookies.sqlite storage file.
    {
        ProfileSettings settingsA(QStringLiteral("wk12a"));
        settingsA.setValue(QStringLiteral("cookies/acceptCookies"),
                           QStringLiteral("AcceptAlways"));
        settingsA.setValue(
            QStringLiteral("cookies/blockThirdPartyCookies"), false);
        settingsA.setValue(QStringLiteral("cookies/keepCookiesUntil"),
                           QStringLiteral("KeepUntilExpire"));
        // The backend reads through a second store — flush this one
        // so the write is visible by session materialization.
        settingsA.sync();
        Engine::ProfileOptions optA;
        optA.storageName = QStringLiteral("arktos-wk12a");
        auto *profileA = qobject_cast<WebkitProfile *>(
            webkit->createProfile(optA, this));
        QVERIFY(profileA);
        Engine::Page *pageA = webkit->createPage(profileA, this);
        QVERIFY2(pageA, "createPage refused profile A");

        // WPE ordinals: ACCEPT_ALWAYS=0 / ACCEPT_NEVER=1 /
        // ACCEPT_NO_THIRD_PARTY=2.
        QCOMPARE(profileA->cookieAcceptPolicy(), 0);
        QVERIFY(profileA->cookieStoragePath().startsWith(
            profileA->sessionDataDir()));
        QVERIFY(profileA->cookieStoragePath().endsWith(
            QStringLiteral("/cookies.sqlite")));

        QVERIFY2(loadSync(pageA, url), "profile A fixture load failed");
        EvalOut write = evalSync(pageA, QStringLiteral(
            "(function(){localStorage.setItem('wk12','alpha');"
            "document.cookie='wk12=alpha;path=/';"
            "return 'done';})()"));
        QVERIFY2(write.done, "storage write eval timed out");
        QCOMPARE(write.value.toString(), QStringLiteral("done"));

        QTRY_VERIFY2_WITH_TIMEOUT(
                cookiesSync(profileA).contains(
                    QStringLiteral("wk12=alpha")),
                "cookie never reached the session store", 30000);
        QCOMPARE(evalSync(pageA, QStringLiteral(
            "(function(){return localStorage.getItem('wk12')||'';})()"))
                .value.toString(),
            QStringLiteral("alpha"));

        // clear(CookiesArea): the manager reads back empty while DOM
        // storage is untouched.
        profileA->clear(Engine::CookiesArea);
        QTRY_VERIFY2_WITH_TIMEOUT(
                cookiesSync(profileA).isEmpty(),
                "clear(CookiesArea) left cookies behind", 30000);
        QCOMPARE(evalSync(pageA, QStringLiteral(
            "(function(){return localStorage.getItem('wk12')||'';})()"))
                .value.toString(),
            QStringLiteral("alpha"));

        // clear(AllAreas): cookies wiped AND a second page on the
        // same session reads an empty localStorage (the live page's
        // in-memory copy is not the store).
        evalSync(pageA, QStringLiteral(
            "(function(){document.cookie='wk12=beta;path=/';"
            "return 'done';})()"));
        QTRY_VERIFY2_WITH_TIMEOUT(
                cookiesSync(profileA).contains(
                    QStringLiteral("wk12=beta")),
                "second cookie write never reached the store", 30000);
        profileA->clear(Engine::AllAreas);
        QTRY_VERIFY2_WITH_TIMEOUT(
                cookiesSync(profileA).isEmpty(),
                "clear(AllAreas) left cookies behind", 30000);
        Engine::Page *pageA2 = webkit->createPage(profileA, this);
        QVERIFY2(pageA2, "createPage refused the second page");
        QVERIFY2(loadSync(pageA2, url), "second page load failed");
        QTRY_COMPARE_EQ_WITH_TIMEOUT(
                evalSync(pageA2, QStringLiteral(
                    "(function(){return localStorage.getItem('wk12')"
                    "||'';})()")).value.toString(),
                QString(), 30000);
        QVERIFY2(!evalSync(pageA2, QStringLiteral(
            "(function(){return document.cookie;})()"))
                .value.toString().contains(QStringLiteral("wk12=beta")),
                "cleared cookie still served to a fresh page");
    }

    // --- Leg 2: accept-policy mapping — AcceptNever -> ACCEPT_NEVER
    // refuses the fixture's cookie outright.
    {
        ProfileSettings settingsB(QStringLiteral("wk12b"));
        settingsB.setValue(QStringLiteral("cookies/acceptCookies"),
                           QStringLiteral("AcceptNever"));
        settingsB.sync();
        Engine::ProfileOptions optB;
        optB.storageName = QStringLiteral("arktos-wk12b");
        auto *profileB = qobject_cast<WebkitProfile *>(
            webkit->createProfile(optB, this));
        QVERIFY(profileB);
        Engine::Page *pageB = webkit->createPage(profileB, this);
        QVERIFY2(pageB, "createPage refused profile B");
        QCOMPARE(profileB->cookieAcceptPolicy(), 1);

        QVERIFY2(loadSync(pageB, url), "profile B fixture load failed");
        evalSync(pageB, QStringLiteral(
            "(function(){document.cookie='wk12=denied;path=/';"
            "return 'done';})()"));
        // The (rejected) write gets a beat to settle — the manager
        // must still read back empty.
        QTest::qWait(1500);
        QVERIFY2(cookiesSync(profileB).isEmpty(),
                 "AcceptNever stored a cookie anyway");
    }

    // --- Leg 3: the KeepUntilExit honesty — memory-only cookies for
    // the session (no persistent-storage file) while the default
    // accept policy maps to NO_THIRD_PARTY.
    {
        ProfileSettings settingsC(QStringLiteral("wk12c"));
        settingsC.setValue(QStringLiteral("cookies/keepCookiesUntil"),
                           QStringLiteral("KeepUntilExit"));
        settingsC.sync();
        Engine::ProfileOptions optC;
        optC.storageName = QStringLiteral("arktos-wk12c");
        auto *profileC = qobject_cast<WebkitProfile *>(
            webkit->createProfile(optC, this));
        QVERIFY(profileC);
        Engine::Page *pageC = webkit->createPage(profileC, this);
        QVERIFY2(pageC, "createPage refused profile C");
        QCOMPARE(profileC->cookieAcceptPolicy(), 2);
        QVERIFY(profileC->cookieStoragePath().isEmpty());
        QVERIFY2(loadSync(pageC, url), "profile C fixture load failed");
        evalSync(pageC, QStringLiteral(
            "(function(){document.cookie='wk12=session;path=/';"
            "return 'done';})()"));
        QTRY_VERIFY2_WITH_TIMEOUT(
                cookiesSync(profileC).contains(
                    QStringLiteral("wk12=session")),
                "session cookie never reached the memory store", 30000);
    }

    // --- Leg 4: the PRIV01 toggle — AcceptAlways +
    // blockThirdPartyCookies maps to NO_THIRD_PARTY, not ALWAYS.
    {
        ProfileSettings settingsD(QStringLiteral("wk12d"));
        settingsD.setValue(QStringLiteral("cookies/acceptCookies"),
                           QStringLiteral("AcceptAlways"));
        settingsD.setValue(
            QStringLiteral("cookies/blockThirdPartyCookies"), true);
        settingsD.sync();
        Engine::ProfileOptions optD;
        optD.storageName = QStringLiteral("arktos-wk12d");
        auto *profileD = qobject_cast<WebkitProfile *>(
            webkit->createProfile(optD, this));
        QVERIFY(profileD);
        Engine::Page *pageD = webkit->createPage(profileD, this);
        QVERIFY2(pageD, "createPage refused profile D");
        QCOMPARE(profileD->cookieAcceptPolicy(), 2);
    }

    // --- Leg 5: OTR — the ephemeral session applies the ambient
    // persona's accept policy but never gets a storage file.
    {
        Engine::ProfileOptions optOtr;
        optOtr.offTheRecord = true;
        auto *profileOtr = qobject_cast<WebkitProfile *>(
            webkit->createProfile(optOtr, this));
        QVERIFY(profileOtr);
        Engine::Page *pageOtr = webkit->createPage(profileOtr, this);
        QVERIFY2(pageOtr, "createPage refused the OTR profile");
        QVERIFY(profileOtr->sessionIsEphemeral());
        QVERIFY(profileOtr->cookieStoragePath().isEmpty());
        QCOMPARE(profileOtr->cookieAcceptPolicy(), 2);
    }
#else
    QSKIP("webkit backend not linked in this build");
#endif
}

// WK13 — the input path: real QWidget event delivery -> wpe-platform
// events -> the web process.  A QTest-synthesized click on the
// fixture link navigates (move+down+up through the live widget),
// keydown reaches the page with the right DOM key/code, Return on a
// focused anchor activates it, both wheel surfaces scroll (angle
// ticks and precise pixel deltas), and a widget resize re-renders
// the next buffer at the new dims.
void tst_EngineSwitch::webkitInput()
{
#ifdef ARKTOS_WEBKIT_LINKED
    Engine::Backend *webkit =
        EngineRegistry::backendForId(QStringLiteral("webkit"));
    QVERIFY(webkit);
    QVERIFY2(webkit->pageUnavailableReason().isEmpty(),
             qPrintable(webkit->pageUnavailableReason()));

    // Fixture: /one carries the click target, a keydown log and a
    // tall scrollable body; /two anchors back to /one.
    const QByteArray pageOne =
        "<html><head><title>Wk13Input</title></head><body>"
        "<a id=\"next\" href=\"/two\" style=\"display:block;"
        "width:200px;height:60px\">two</a>"
        "<div style=\"height:4000px\"></div>"
        "<script>window.__keys=[];window.__wheels=[];"
        "addEventListener('keydown',function(e){"
        "__keys.push(e.key+'|'+e.code);});"
        "addEventListener('wheel',function(e){"
        "__wheels.push(e.deltaY+'|'+e.deltaMode);});"
        "</script></body></html>";
    const QByteArray pageTwo =
        "<html><head><title>Wk13Two</title></head><body>"
        "<a id=\"back\" href=\"/one\">back</a></body></html>";
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    auto pending =
        QSharedPointer<QHash<QTcpSocket *, QByteArray>>::create();
    connect(&server, &QTcpServer::newConnection, &server,
            [&server, pageOne, pageTwo, pending]() {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            socket->setParent(&server);
            connect(socket, &QTcpSocket::readyRead, socket,
                    [socket, pageOne, pageTwo, pending]() {
                QByteArray &request = (*pending)[socket];
                request += socket->readAll();
                if (!request.contains("\r\n\r\n"))
                    return;
                const QByteArray body =
                    request.startsWith("GET /two") ? pageTwo : pageOne;
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html"
                              "\r\nCache-Control: no-store"
                              "\r\nContent-Length: "
                              + QByteArray::number(body.size())
                              + "\r\nConnection: close\r\n\r\n" + body);
                socket->disconnect();
                socket->disconnectFromHost();
                pending->remove(socket);
            });
        }
    });
    const QUrl urlOne(QStringLiteral("http://127.0.0.1:%1/one")
                          .arg(server.serverPort()));
    const QUrl urlTwo(QStringLiteral("http://127.0.0.1:%1/two")
                          .arg(server.serverPort()));

    Engine::ProfileOptions options;
    options.offTheRecord = true;   // ephemeral session — no disk state
    Engine::Profile *profile = webkit->createProfile(options, this);
    QVERIFY(profile);

    EngineTab tab(webkit, profile);
    tab.resize(800, 600);
    tab.show();
    QVERIFY(tab.page());
    auto *page = qobject_cast<WebkitPage *>(tab.page());
    QVERIFY(page);
    auto *view = qobject_cast<WebkitView *>(tab.page()->view());
    QVERIFY(view);

    struct EvalOut { QVariant value; bool done = false; };
    auto evalSync = [&page](const QString &src) -> EvalOut {
        EvalOut out;
        page->runJavaScript(src, [&](const QVariant &v) {
            out.value = v;
            out.done = true;
        });
        QElapsedTimer timer;
        timer.start();
        while (!out.done && timer.elapsed() < 30000)
            QTest::qWait(20);
        return out;
    };
    auto loadSync = [&page](const QUrl &u) {
        QSignalSpy spy(page, &Engine::Page::loadFinished);
        page->load(u);
        return spy.wait(60000) && spy.first().first().toBool();
    };

    QVERIFY2(loadSync(urlOne), "fixture /one load failed");
    view->setFocus();

    // --- keydown log: key events reach the page with the right DOM
    // key + code — 'a' unshifted and 'B' under Shift.
    QTest::keyClick(view, Qt::Key_A);
    QTest::keyClick(view, Qt::Key_B, Qt::ShiftModifier);
    QTRY_VERIFY2_WITH_TIMEOUT(
            evalSync(QStringLiteral("window.__keys.join(',')"))
                .value.toString().contains(
                    QStringLiteral("a|KeyA"))
            && evalSync(QStringLiteral("window.__keys.join(',')"))
                .value.toString().contains(
                    QStringLiteral("B|KeyB")),
            "keydown events never reached the page (or arrived with "
            "the wrong DOM key/code)", 30000);

    // --- wheel: angleDelta ticks scroll down; a precise pixelDelta
    // event takes the pixel path.
    QCOMPARE(evalSync(QStringLiteral(
                 "(function(){return Math.round(window.scrollY);})()"))
                 .value.toInt(), 0);
    {
        const QPointF pos(400, 300);
        QWheelEvent wheel(pos, view->mapToGlobal(pos.toPoint()),
                          QPoint(0, 0), QPoint(0, -120),
                          Qt::NoButton, Qt::NoModifier,
                          Qt::NoScrollPhase, false);
        QApplication::sendEvent(view, &wheel);
    }
    QTRY_VERIFY2_WITH_TIMEOUT(
            evalSync(QStringLiteral(
                "(function(){return Math.round(window.scrollY);})()"))
                .value.toInt() > 0,
            "wheel tick never scrolled the page", 30000);
    const int afterTick = evalSync(QStringLiteral(
        "(function(){return Math.round(window.scrollY);})()"))
        .value.toInt();
    {
        const QPointF pos(400, 300);
        // pixelDelta -100 = 100 real pixels of downward scroll —
        // the precise-delta path (Qt sign convention: negative is
        // toward the user = scroll down).
        QWheelEvent wheel(pos, view->mapToGlobal(pos.toPoint()),
                          QPoint(0, -100), QPoint(0, 0),
                          Qt::NoButton, Qt::NoModifier,
                          Qt::NoScrollPhase, false);
        QApplication::sendEvent(view, &wheel);
    }
    QTRY_VERIFY2_WITH_TIMEOUT(
            evalSync(QStringLiteral(
                "(function(){return Math.round(window.scrollY);})()"))
                .value.toInt() > afterTick,
            "precise pixel-delta scroll never moved", 30000);

    // --- click: move + down + up on the link navigates to /two.
    // Back to top first — the wheel legs scrolled the link
    // off-viewport.
    evalSync(QStringLiteral("window.scrollTo(0,0)"));
    QTest::qWait(300);
    const EvalOut rect = evalSync(QStringLiteral(
        "(function(){var r=document.getElementById('next')"
        ".getBoundingClientRect();"
        "return r.x+','+r.y+','+r.width+','+r.height;})()"));
    QVERIFY2(rect.done, "link rect eval timed out");
    const QStringList parts = rect.value.toString().split(
        QLatin1Char(','));
    QCOMPARE(parts.size(), 4);
    const QPoint click(
        int(parts.at(0).toDouble() + parts.at(2).toDouble() / 2.0),
        int(parts.at(1).toDouble() + parts.at(3).toDouble() / 2.0));
    {
        QSignalSpy spy(page, &Engine::Page::loadFinished);
        QTest::mouseMove(view, click);
        QTest::mouseClick(view, Qt::LeftButton, Qt::NoModifier, click);
        QVERIFY2(spy.wait(60000) && spy.first().first().toBool(),
                 "click navigation never finished");
    }
    QCOMPARE(page->url(), urlTwo);

    // --- keyboard activation: focus the back anchor via JS, then a
    // Return down/up pair navigates home.
    const EvalOut focused = evalSync(QStringLiteral(
        "document.getElementById('back').focus();"
        "document.activeElement && document.activeElement.id"));
    QCOMPARE(focused.value.toString(), QStringLiteral("back"));
    {
        QSignalSpy spy(page, &Engine::Page::loadFinished);
        QTest::keyClick(view, Qt::Key_Return);
        QVERIFY2(spy.wait(60000) && spy.first().first().toBool(),
                 "Return activation never finished");
    }
    QCOMPARE(page->url(), urlOne);

    // --- resize: the next produced buffer carries the new dims.
    tab.resize(640, 480);
    QTRY_COMPARE_EQ_WITH_TIMEOUT(view->frameSize(),
                                 QSize(640, 480), 30000);
#else
    QSKIP("webkit backend not linked in this build");
#endif
}

// WK14 — zoom, find-in-page, and the lifecycle surface.  Zoom is
// asserted on the render scale itself (the CSS viewport width shrinks
// at zoom > 1) plus the zoom_factor read-back.  Find exercises the
// WebKitFindController path: the case-insensitive/count contract, a
// repeat query stepping instead of re-searching, the backward flag,
// the miss, and the empty-needle clear.  Lifecycle proves the
// symbol-gated mapping: on a build without webkit_web_view_suspend
// (2.54) Frozen is the unmap primitive — asserted through the
// diagnostic — and Discarded is terminate_web_process + reload on
// wake, proven by a JS marker surviving the freeze but vanishing
// across the discard.
void tst_EngineSwitch::webkitZoomFindLifecycle()
{
#ifdef ARKTOS_WEBKIT_LINKED
    Engine::Backend *webkit =
        EngineRegistry::backendForId(QStringLiteral("webkit"));
    QVERIFY(webkit);
    QVERIFY2(webkit->pageUnavailableReason().isEmpty(),
             qPrintable(webkit->pageUnavailableReason()));

    const QString fixturePath = QDir::temp().filePath(
        QStringLiteral("arktos-wk14-fixture.html"));
    {
        QFile fixture(fixturePath);
        QVERIFY(fixture.open(QIODevice::WriteOnly));
        fixture.write("<html><head><title>Wk14</title></head>"
                      "<body><p>needle one</p><p>NEEDLE two</p>"
                      "<p>needle three</p></body></html>");
    }
    const QUrl url = QUrl::fromLocalFile(fixturePath);

    Engine::ProfileOptions options;
    options.offTheRecord = true;   // ephemeral session — no disk state
    Engine::Profile *profile = webkit->createProfile(options, this);
    QVERIFY(profile);

    EngineTab tab(webkit, profile);
    tab.resize(800, 600);
    tab.show();
    QVERIFY(tab.page());
    auto *page = qobject_cast<WebkitPage *>(tab.page());
    QVERIFY(page);

    struct EvalOut { QVariant value; bool done = false; };
    auto evalSync = [&page](const QString &src) -> EvalOut {
        EvalOut out;
        page->runJavaScript(src, [&](const QVariant &v) {
            out.value = v;
            out.done = true;
        });
        QElapsedTimer timer;
        timer.start();
        while (!out.done && timer.elapsed() < 30000)
            QTest::qWait(20);
        return out;
    };

    {
        QSignalSpy spy(page, &Engine::Page::loadFinished);
        page->load(url);
        QVERIFY2(spy.wait(60000) && spy.first().first().toBool(),
                 "fixture load failed");
    }
    QCOMPARE(page->url(), url);

    // --- zoom: the read-back reports the new level and the CSS
    // viewport shrinks in proportion (real render-scale change, not
    // just a stored number).  A non-positive factor is refused.
    QCOMPARE(page->zoomFactor(), qreal(1.0));
    const int innerBefore = evalSync(QStringLiteral("window.innerWidth"))
                                .value.toInt();
    QVERIFY2(innerBefore > 0, "innerWidth eval failed");
    page->setZoomFactor(1.5);
    QCOMPARE(page->zoomFactor(), qreal(1.5));
    QTRY_VERIFY2_WITH_TIMEOUT(
            evalSync(QStringLiteral("window.innerWidth")).value.toInt()
                < innerBefore,
            "zoom did not shrink the CSS viewport", 30000);
    page->setZoomFactor(0.0);
    QCOMPARE(page->zoomFactor(), qreal(1.5));   // refused, still 1.5
    page->setZoomFactor(1.0);
    QTRY_COMPARE_EQ_WITH_TIMEOUT(
            evalSync(QStringLiteral("window.innerWidth")).value.toInt(),
            innerBefore, 30000);

    // --- find: case-insensitive default counts all three spellings.
    QSignalSpy findSpy(page, &Engine::Page::findTextFinished);
    auto findSync = [&](const QString &needle,
                        Engine::FindFlags flags) -> Engine::FindResult {
        const int before = findSpy.count();
        page->findText(needle, flags);
        QElapsedTimer timer;
        timer.start();
        // The last emission wins — a corrected count can follow on
        // counted-matches, so settle on the newest entry.
        while (findSpy.count() == before && timer.elapsed() < 30000)
            QTest::qWait(20);
        if (findSpy.count() == before)
            return Engine::FindResult{};
        // One extra beat for a counted-matches correction.
        QTest::qWait(300);
        return findSpy.last().first().value<Engine::FindResult>();
    };
    Engine::FindResult found =
        findSync(QStringLiteral("needle"), Engine::FindFlags());
    QCOMPARE(found.numberOfMatches, 3);
    QCOMPARE(found.activeMatch, 1);
    // The repeat call steps to the next match instead of restarting.
    found = findSync(QStringLiteral("needle"), Engine::FindFlags());
    QCOMPARE(found.numberOfMatches, 3);
    QCOMPARE(found.activeMatch, 2);
    // Case-sensitive honors the flag: the uppercase NEEDLE drops out.
    found = findSync(QStringLiteral("needle"),
                     Engine::FindCaseSensitively);
    QCOMPARE(found.numberOfMatches, 2);
    // Backward fresh search resolves to the last match.
    found = findSync(QStringLiteral("needle"), Engine::FindBackward);
    QCOMPARE(found.numberOfMatches, 3);
    QCOMPARE(found.activeMatch, 3);
    // The miss reports zero; the empty needle clears the search.
    found = findSync(QStringLiteral("definitely-absent"),
                     Engine::FindFlags());
    QCOMPARE(found.numberOfMatches, 0);
    found = findSync(QString(), Engine::FindFlags());
    QCOMPARE(found.numberOfMatches, 0);

    // --- lifecycle.  Log the gate verdict: 2.54 ships no
    // suspend/resume exports — Frozen must be the unmap fallback.
    qInfo("webkit suspend/resume exports: %s",
          WebkitPage::suspendResumeAvailable() ? "yes" : "no");
    QVERIFY(page->engineViewMapped());
    QCOMPARE(evalSync(QStringLiteral(
                 "window.__wk14='alive';'set'")).value.toString(),
             QStringLiteral("set"));

    // Frozen: the page survives (eval answers, marker intact) — on a
    // build without the suspend export the honest primitive is the
    // unmapped view.
    page->setLifecycleState(Engine::Page::LifecycleState::Frozen);
    QCOMPARE(page->lifecycleState(), Engine::Page::LifecycleState::Frozen);
    if (!WebkitPage::suspendResumeAvailable())
        QVERIFY2(!page->engineViewMapped(),
                 "Frozen without suspend did not unmap the view");
    QCOMPARE(evalSync(QStringLiteral("window.__wk14||'gone'"))
                 .value.toString(),
             QStringLiteral("alive"));
    page->setLifecycleState(Engine::Page::LifecycleState::Active);
    QCOMPARE(page->lifecycleState(), Engine::Page::LifecycleState::Active);
    QVERIFY(page->engineViewMapped());
    QCOMPARE(evalSync(QStringLiteral("window.__wk14||'gone'"))
                 .value.toString(),
             QStringLiteral("alive"));

    // Discarded: the web process actually dies — the marker is gone
    // after the wake reload, and the UI-side history entry survives
    // (url unchanged, loadFinished(true) on reactivation).
    page->setLifecycleState(Engine::Page::LifecycleState::Discarded);
    QCOMPARE(page->lifecycleState(),
             Engine::Page::LifecycleState::Discarded);
    QVERIFY(page->engineProcessDiscarded());
    {
        QSignalSpy spy(page, &Engine::Page::loadFinished);
        page->setLifecycleState(Engine::Page::LifecycleState::Active);
        QVERIFY2(spy.wait(60000) && spy.first().first().toBool(),
                 "wake-from-discard reload never finished");
    }
    QCOMPARE(page->lifecycleState(), Engine::Page::LifecycleState::Active);
    QVERIFY(!page->engineProcessDiscarded());
    QCOMPARE(page->url(), url);
    QTRY_COMPARE_EQ_WITH_TIMEOUT(
            evalSync(QStringLiteral("window.__wk14||'gone'"))
                .value.toString(),
            QStringLiteral("gone"), 30000);
#else
    QSKIP("webkit backend not linked in this build");
#endif
}

// WK15 — the typed-result contract behind runJavaScript.  Scalar
// JSCValues marshal to their QVariant counterpart, JS null is a
// valid-but-null variant (QMetaType::Nullptr, like QJsonValue::Null),
// undefined is the invalid variant, objects/arrays arrive as their
// JSON text (the documented limit), and a thrown exception or syntax
// error surfaces as an invalid result through the callback — the
// same shape QtWebEngine reports, never a crash or a dropped call.
// toHtml is the outerHTML eval over the same channel.
void tst_EngineSwitch::webkitJsEval()
{
#ifdef ARKTOS_WEBKIT_LINKED
    Engine::Backend *webkit =
        EngineRegistry::backendForId(QStringLiteral("webkit"));
    QVERIFY(webkit);
    QVERIFY2(webkit->pageUnavailableReason().isEmpty(),
             qPrintable(webkit->pageUnavailableReason()));

    const QString fixturePath = QDir::temp().filePath(
        QStringLiteral("arktos-wk15-fixture.html"));
    {
        QFile fixture(fixturePath);
        QVERIFY(fixture.open(QIODevice::WriteOnly));
        fixture.write("<html><head><title>Wk15</title></head>"
                      "<body><p id='wk15mark'>eval me</p></body></html>");
    }

    Engine::ProfileOptions options;
    options.offTheRecord = true;
    Engine::Profile *profile = webkit->createProfile(options, this);
    QVERIFY(profile);

    EngineTab tab(webkit, profile);
    tab.resize(800, 600);
    tab.show();
    QVERIFY(tab.page());
    auto *page = qobject_cast<WebkitPage *>(tab.page());
    QVERIFY(page);

    struct EvalOut { QVariant value; bool done = false; };
    auto evalSync = [&page](const QString &src) -> EvalOut {
        EvalOut out;
        page->runJavaScript(src, [&](const QVariant &v) {
            out.value = v;
            out.done = true;
        });
        QElapsedTimer timer;
        timer.start();
        while (!out.done && timer.elapsed() < 30000)
            QTest::qWait(20);
        return out;
    };

    {
        QSignalSpy spy(page, &Engine::Page::loadFinished);
        page->load(QUrl::fromLocalFile(fixturePath));
        QVERIFY2(spy.wait(60000) && spy.first().first().toBool(),
                 "fixture load failed");
    }

    // Scalars round-trip with their types intact.
    EvalOut out = evalSync(QStringLiteral("40+2"));
    QVERIFY(out.done);
    QVERIFY(out.value.isValid());
    QCOMPARE(out.value.typeId(), QMetaType::Double);
    QCOMPARE(out.value.toInt(), 42);

    out = evalSync(QStringLiteral("'ark'+'tos'"));
    QVERIFY(out.value.isValid());
    QCOMPARE(out.value.typeId(), QMetaType::QString);
    QCOMPARE(out.value.toString(), QStringLiteral("arktos"));

    out = evalSync(QStringLiteral("1<2"));
    QVERIFY(out.value.isValid());
    QCOMPARE(out.value.typeId(), QMetaType::Bool);
    QCOMPARE(out.value.toBool(), true);
    out = evalSync(QStringLiteral("1>2"));
    QCOMPARE(out.value.toBool(), false);

    // The completion value of a statement list is its last
    // expression — the eval semantics WebKit and WebEngine share.
    out = evalSync(QStringLiteral("var a=6; a*7"));
    QVERIFY(out.value.isValid());
    QCOMPARE(out.value.toInt(), 42);

    // null vs undefined: both falsy, but null is a marshaled VALUE
    // (valid, QMetaType::Nullptr) while undefined is the invalid
    // variant — the distinction the interface's callers rely on.
    out = evalSync(QStringLiteral("null"));
    QVERIFY(out.value.isValid());
    QVERIFY(out.value.isNull());
    QCOMPARE(out.value.typeId(), QMetaType::Nullptr);
    out = evalSync(QStringLiteral("undefined"));
    QVERIFY(!out.value.isValid());

    // Objects/arrays arrive as JSON text — the documented marshaling
    // limit (WebEngine decodes to Map/List; this backend hands back
    // the serialized form instead of faking structure).
    out = evalSync(QStringLiteral("({n:7,s:'x'})"));
    QVERIFY(out.value.isValid());
    QCOMPARE(out.value.typeId(), QMetaType::QString);
    const QJsonDocument objectDoc =
        QJsonDocument::fromJson(out.value.toString().toUtf8());
    QVERIFY(objectDoc.isObject());
    QCOMPARE(objectDoc.object().value(QLatin1String("n")).toInt(), 7);
    QCOMPARE(objectDoc.object().value(QLatin1String("s")).toString(),
             QStringLiteral("x"));

    out = evalSync(QStringLiteral("[1,2,3]"));
    QVERIFY(out.value.isValid());
    const QJsonDocument arrayDoc =
        QJsonDocument::fromJson(out.value.toString().toUtf8());
    QVERIFY(arrayDoc.isArray());
    QCOMPARE(arrayDoc.array().size(), 3);

    // Error propagation: a thrown exception and a parse error both
    // answer the callback with the invalid variant — error surfaced,
    // never a crash and never a silently dropped call.
    out = evalSync(QStringLiteral("throw new Error('wk15-boom')"));
    QVERIFY(out.done);
    QVERIFY(!out.value.isValid());
    out = evalSync(QStringLiteral("1 +"));
    QVERIFY(out.done);
    QVERIFY(!out.value.isValid());

    // toHtml is the outerHTML eval — serialized markup reaches the
    // QString callback, fixture content included.
    QString markup;
    bool htmlDone = false;
    page->toHtml([&](const QString &html) {
        markup = html;
        htmlDone = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(htmlDone, 30000);
    QVERIFY2(markup.contains(QStringLiteral("<html")),
             qPrintable(markup.left(200)));
    QVERIFY(markup.contains(QStringLiteral("wk15mark")));
#else
    QSKIP("webkit backend not linked in this build");
#endif
}

// WK16 — the script-injection + named-world + host-channel contract.
// insertScript reaches the page's WebKitUserContentManager with the
// injection-time and world-name mappings applied; runJavaScriptLifted
// evaluates in the named world; the "arktosChannel" script-message
// handler round-trips a message through the installed listener; and
// the SEC24 isolation shape holds — main-world page script can
// neither see the channel world's objects nor enumerate the
// messageHandlers surface.
void tst_EngineSwitch::webkitScriptWorlds()
{
#ifdef ARKTOS_WEBKIT_LINKED
    Engine::Backend *webkit =
        EngineRegistry::backendForId(QStringLiteral("webkit"));
    QVERIFY(webkit);
    QVERIFY2(webkit->pageUnavailableReason().isEmpty(),
             qPrintable(webkit->pageUnavailableReason()));

    // The world-name map is the contract's public half: world 0 is
    // the main world, ArktosChannelWorld the stable channel name,
    // other ids deterministic.
    QVERIFY(WebkitPage::scriptWorldName(0).isEmpty());
    QCOMPARE(WebkitPage::scriptWorldName(Engine::ArktosChannelWorld),
             QByteArrayLiteral("arktos-channel"));
    QCOMPARE(WebkitPage::scriptWorldName(0x99),
             QByteArrayLiteral("arktos-world-99"));
    QVERIFY(!WebkitPage::channelHandlerName().isEmpty());
    qInfo("webkit script-message _with_reply export: %s",
          WebkitPage::scriptMessageReplyAvailable() ? "yes" : "no");

    const QString fixturePath = QDir::temp().filePath(
        QStringLiteral("arktos-wk16-fixture.html"));
    {
        QFile fixture(fixturePath);
        QVERIFY(fixture.open(QIODevice::WriteOnly));
        fixture.write("<html><head><title>Wk16</title>"
                      "<script>window.__pageState=document.readyState;"
                      "</script></head>"
                      "<body><p id='wk16mark'>inject me</p>"
                      "</body></html>");
    }

    Engine::ProfileOptions options;
    options.offTheRecord = true;
    Engine::Profile *profile = webkit->createProfile(options, this);
    QVERIFY(profile);
    // A profile-scope script installed before the page exists must
    // land on the page's own UCM — the construction-time half of the
    // propagation contract.
    profile->insertScript(Engine::Script{
        QStringLiteral("wk16:profile-early"),
        QStringLiteral("window.__wk16prof='early';"),
        Engine::InjectionPoint::Deferred, 0 /* main world */, false });

    EngineTab tab(webkit, profile);
    tab.resize(800, 600);
    tab.show();
    QVERIFY(tab.page());
    auto *page = qobject_cast<WebkitPage *>(tab.page());
    QVERIFY(page);

    // The native endpoint: record the payload and answer it — the
    // return QVariant resolves the JS-side Promise.
    QStringList received;
    page->setChannelMessageListener(
        [&received](const QVariant &payload) {
            received << payload.toString();
            return QStringLiteral("ack:%1")
                .arg(payload.toString().toUpper());
        });

    // Channel-world script at DocumentCreation: marks the world,
    // records the injection-time readyState, and round-trips a
    // channel message.  Installed BEFORE the load so document-start
    // injection actually fires.
    const QString handler =
        QString::fromLatin1(WebkitPage::channelHandlerName());
    Engine::Script channelScript;
    channelScript.name = QStringLiteral("wk16:channel-marker");
    channelScript.injectionPoint = Engine::InjectionPoint::DocumentCreation;
    channelScript.worldId = Engine::ArktosChannelWorld;
    channelScript.runsOnSubFrames = false;
    channelScript.sourceCode = QStringLiteral(
        "window.__wk16mark='created';"
        "window.__wk16state=document.readyState;"
        "(function(){"
        "  var h=window.webkit&&window.webkit.messageHandlers"
        "    &&window.webkit.messageHandlers.%1;"
        "  if(!h){window.__wk16reply='no-handler';return;}"
        "  var p=h.postMessage('wk16');"
        "  if(p&&p.then){p.then(function(r){window.__wk16reply=r;},"
        "    function(e){window.__wk16reply='rejected:'+e;});}"
        "  else{window.__wk16reply='no-promise';}"
        "}())").arg(handler);
    page->insertScript(channelScript);
    // A deferred main-world script for the injection-point split.
    page->insertScript(Engine::Script{
        QStringLiteral("wk16:main-deferred"),
        QStringLiteral("window.__wk16main=document.readyState;"),
        Engine::InjectionPoint::Deferred, 0 /* main world */, false });
    // A third world — a world name only becomes durable once a user
    // script anchors it; eval-only names are transient per call.
    page->insertScript(Engine::Script{
        QStringLiteral("wk16:third-world"),
        QStringLiteral("window.__third=7;"),
        Engine::InjectionPoint::DocumentCreation, 0x99, false });
    QCOMPARE(page->scripts().size(), 3);

    {
        QSignalSpy spy(page, &Engine::Page::loadFinished);
        page->load(QUrl::fromLocalFile(fixturePath));
        QVERIFY2(spy.wait(60000) && spy.first().first().toBool(),
                 "fixture load failed");
    }

    auto evalW = [&page](const QString &src,
                         quint32 world) -> QVariant {
        QVariant out;
        bool done = false;
        page->runJavaScriptLifted(src, [&](const QVariant &v) {
            out = v;
            done = true;
        }, world);
        QElapsedTimer timer;
        timer.start();
        while (!done && timer.elapsed() < 30000)
            QTest::qWait(20);
        return out;
    };
    const quint32 channel = Engine::ArktosChannelWorld;
    const quint32 main = 0;

    // The injected script ran at document creation in the named
    // world — the marker is only reachable there.
    QCOMPARE(evalW(QStringLiteral("window.__wk16mark"), channel)
                 .toString(),
             QStringLiteral("created"));
    QCOMPARE(evalW(QStringLiteral("window.__wk16state"), channel)
                 .toString(),
             QStringLiteral("loading"));
    // SEC24: the main world cannot see the channel world's objects.
    QCOMPARE(evalW(QStringLiteral("typeof window.__wk16mark"), main)
                 .toString(),
             QStringLiteral("undefined"));
    // The deferred main-world script landed at the document-end
    // boundary in the main world.
    const QString mainState =
        evalW(QStringLiteral("window.__wk16main"), main).toString();
    QVERIFY2(mainState == QStringLiteral("interactive")
                 || mainState == QStringLiteral("complete"),
             qPrintable(mainState));
    // worldId selects a genuinely distinct world — the third world's
    // counter is durable (anchored by its user script) and never
    // leaks to main.
    QCOMPARE(evalW(QStringLiteral("window.__third"), 0x99).toInt(), 7);
    QCOMPARE(evalW(QStringLiteral("typeof window.__third"), main)
                 .toString(),
             QStringLiteral("undefined"));
    // The profile-scope script armed before page construction ran.
    QCOMPARE(evalW(QStringLiteral("window.__wk16prof"), main)
                 .toString(),
             QStringLiteral("early"));

    // The message round-trip: the listener saw the payload and the
    // Promise resolved with its return value.
    QTRY_VERIFY2_WITH_TIMEOUT(received.contains(QStringLiteral("wk16")),
                              "channel message never reached the "
                              "listener", 30000);
    if (WebkitPage::scriptMessageReplyAvailable()) {
        QTRY_COMPARE_EQ_WITH_TIMEOUT(
                evalW(QStringLiteral("window.__wk16reply||''"), channel)
                    .toString(),
                QStringLiteral("ack:WK16"), 30000);
    } else {
        QTRY_VERIFY2_WITH_TIMEOUT(
                evalW(QStringLiteral("window.__wk16reply||''"), channel)
                    .toString() == QStringLiteral("no-promise"),
                "one-way handler still delivered no promise", 30000);
    }
    // SEC24: main-world script cannot enumerate the channel surface —
    // window.webkit.messageHandlers never materializes in a world
    // with no handler registered.
    QCOMPARE(evalW(QStringLiteral(
                 "!!(window.webkit&&window.webkit.messageHandlers"
                 "&&window.webkit.messageHandlers.%1)").arg(handler),
                 main).toBool(),
             false);

    // removeScript retires the injection: reload and the channel
    // world is bare again.
    page->removeScript(QStringLiteral("wk16:channel-marker"));
    QCOMPARE(page->scripts().size(), 2);
    {
        QSignalSpy spy(page, &Engine::Page::loadFinished);
        page->reload();
        QVERIFY2(spy.wait(60000) && spy.first().first().toBool(),
                 "reload after removeScript failed");
    }
    QCOMPARE(evalW(QStringLiteral("typeof window.__wk16mark"), channel)
                 .toString(),
             QStringLiteral("undefined"));

    // Live profile propagation: a profile script inserted after the
    // page exists reaches it on the next load; removing it takes it
    // back out.
    profile->insertScript(Engine::Script{
        QStringLiteral("wk16:profile-live"),
        QStringLiteral("window.__wk16live='propagated';"),
        Engine::InjectionPoint::Deferred, 0 /* main world */, false });
    {
        QSignalSpy spy(page, &Engine::Page::loadFinished);
        page->reload();
        QVERIFY2(spy.wait(60000) && spy.first().first().toBool(),
                 "reload after profile insert failed");
    }
    QCOMPARE(evalW(QStringLiteral("window.__wk16live"), main)
                 .toString(),
             QStringLiteral("propagated"));
    profile->removeScript(QStringLiteral("wk16:profile-live"));
    {
        QSignalSpy spy(page, &Engine::Page::loadFinished);
        page->reload();
        QVERIFY2(spy.wait(60000) && spy.first().first().toBool(),
                 "reload after profile remove failed");
    }
    QCOMPARE(evalW(QStringLiteral("typeof window.__wk16live"), main)
                 .toString(),
             QStringLiteral("undefined"));
#else
    QSKIP("webkit backend not linked in this build");
#endif
}

// WK17 — decide-policy -> RequestPolicy + the custom-scheme serving
// set: a fixture policy consulted as a main-frame navigation vetoes a
// marker URL that never reaches the wire; a file:// directory nav
// reroutes onto arktos-file: and the scheme serves the real listing;
// the interstitial nonce registry and the compiled stub table serve
// backend-0's payloads; abp:subscribe reaches the persona's
// AdBlockManager prompt.
void tst_EngineSwitch::webkitPolicySchemes()
{
#ifdef ARKTOS_WEBKIT_LINKED
    Engine::Backend *webkit =
        EngineRegistry::backendForId(QStringLiteral("webkit"));
    QVERIFY(webkit);
    QVERIFY2(webkit->pageUnavailableReason().isEmpty(),
             qPrintable(webkit->pageUnavailableReason()));
    // The veto consult is the WK17 contract — the capability must be
    // on for this backend.
    QVERIFY(webkit->capabilities().requestInterception);

    // Fixture policy: records every consultation and vetoes the
    // marker URL — the "blocked nav never loads" leg's driver.
    class FixturePolicy : public Engine::RequestPolicy
    {
    public:
        Engine::RequestDecision decide(
            const Engine::NavigationRequest &request) override
        {
            urls << request.url;
            if (request.isMainFrame)
                ++mainFrameConsults;
            Engine::RequestDecision out;
            if (request.url == denied)
                out.action = Engine::RequestAction::Block;
            return out;
        }
        QList<QUrl> urls;
        int mainFrameConsults = 0;
        QUrl denied;
    };

    Engine::ProfileOptions options;
    options.offTheRecord = true;   // ephemeral session — no disk state
    Engine::Profile *profile = webkit->createProfile(options, this);
    QVERIFY(profile);
    FixturePolicy policy;
    profile->setRequestPolicy(&policy);

    EngineTab tab(webkit, profile);
    tab.resize(800, 600);
    tab.show();
    QVERIFY(tab.page());
    auto *page = qobject_cast<WebkitPage *>(tab.page());
    QVERIFY(page);

    // The loopback fixture records requested paths — a vetoed
    // navigation must never produce one for its URL (favicon fetches
    // from the allowed page are noise the path set filters out).
    QSet<QString> wirePaths;
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    auto pending =
        QSharedPointer<QHash<QTcpSocket *, QByteArray>>::create();
    connect(&server, &QTcpServer::newConnection, &server,
            [&server, &wirePaths, pending]() {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            socket->setParent(&server);
            connect(socket, &QTcpSocket::readyRead, socket,
                    [socket, &wirePaths, pending]() {
                QByteArray &request = (*pending)[socket];
                request += socket->readAll();
                if (!request.contains("\r\n\r\n"))
                    return;
                const int firstLineEnd = request.indexOf('\n');
                if (firstLineEnd > 0) {
                    const QByteArray line =
                        request.left(firstLineEnd).trimmed();
                    const QList<QByteArray> parts = line.split(' ');
                    if (parts.size() >= 2)
                        wirePaths.insert(
                            QString::fromUtf8(parts.at(1)));
                }
                const QByteArray body =
                    "<html><head><title>Wk17Fixture</title></head>"
                    "<body>wk17 policy fixture</body></html>";
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html"
                              "\r\nContent-Length: "
                              + QByteArray::number(body.size())
                              + "\r\nConnection: close\r\n\r\n" + body);
                socket->disconnect();
                socket->disconnectFromHost();
                pending->remove(socket);
            });
        }
    });
    const QUrl fixtureUrl(QStringLiteral("http://127.0.0.1:%1/wk17")
                              .arg(server.serverPort()));
    policy.denied = QUrl(QStringLiteral("http://127.0.0.1:%1/denied")
                             .arg(server.serverPort()));

    QList<bool> loadResults;   // every loadFinished emission
    connect(page, &Engine::Page::loadFinished, page,
            [&](bool ok) { loadResults << ok; });
    // Waits for a NEW loadFinished(true) past the marker — an ignored
    // policy decision can still emit loadFinished(false) first.
    auto waitLoadOk = [&](int timeout = 60000) {
        const int base = loadResults.size();
        for (int waited = 0; waited < timeout; waited += 100) {
            for (int i = base; i < loadResults.size(); ++i)
                if (loadResults.at(i))
                    return true;
            QTest::qWait(100);
        }
        return false;
    };

    // — allow leg: the fixture page loads with the policy consulted
    //   as a main-frame navigation.
    tab.load(fixtureUrl);
    QVERIFY2(waitLoadOk(), "fixture load never finished");
    QVERIFY(policy.urls.contains(fixtureUrl));
    QVERIFY(policy.mainFrameConsults > 0);

    // — deny leg: the marker load is vetoed and never reaches the
    //   wire.  An ignored decision may or may not surface a failed
    //   loadFinished — the wire path set is the contract (favicon
    //   fetches of the fixture page are fine; "/denied" is not).
    const int resultsBefore = loadResults.size();
    page->load(policy.denied);
    QTest::qWait(2500);   // settle any stray load/fetch attempt
    QVERIFY2(!wirePaths.contains(QStringLiteral("/denied")),
             "a vetoed navigation reached the wire");
    QVERIFY(policy.urls.contains(policy.denied));
    QVERIFY(page->url() != policy.denied);
    for (int i = resultsBefore; i < loadResults.size(); ++i)
        QVERIFY2(!loadResults.at(i),
                 "a vetoed navigation reported loadFinished(true)");

    // — file:// directory -> arktos-file: reroute + the real listing.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QFile marker(dir.filePath(QStringLiteral("wk17-marker.txt")));
        QVERIFY(marker.open(QIODevice::WriteOnly));
        marker.write("x");
        marker.close();
    }
    page->load(QUrl::fromLocalFile(dir.path()));
    QVERIFY2(waitLoadOk(), "directory listing load never finished");
    QCOMPARE(page->url().scheme(), QStringLiteral("arktos-file"));
    auto pageHtml = [&]() {
        std::shared_ptr<QString> html(new QString);
        std::shared_ptr<bool> done(new bool(false));
        page->toHtml([html, done](const QString &h) {
            *html = h;
            *done = true;
        });
        for (int waited = 0; !*done && waited < 15000; waited += 50)
            QTest::qWait(50);
        return *html;
    };
    QVERIFY(pageHtml().contains(QStringLiteral("wk17-marker.txt")));

    // — interstitial registry: publish + serve (the same nonce store
    //   the cert-error/https-warning/site-block paths feed).
    SchemeAccessHandler::publishInterstitialPage(
        QStringLiteral("wk17nonce"),
        QStringLiteral("<html><body>WK17-INTERSTITIAL</body></html>"));
    page->load(QUrl(QStringLiteral(
        "arktos-cert-error:interstitial?n=wk17nonce")));
    QVERIFY2(waitLoadOk(), "interstitial load never finished");
    QVERIFY(pageHtml().contains(QStringLiteral("WK17-INTERSTITIAL")));

    // — the compiled stub table serves (backend-0's $redirect
    //   payloads route through the same resourceFor).
    page->load(AdBlockResourceHandler::urlForResource("noop.html"));
    QVERIFY2(waitLoadOk(), "arktos-resource stub never finished");

    // — abp:subscribe reaches the persona manager's prompt
    //   (ModalAnswer declines); nothing is served.  Scoped so its
    //   answerer cannot steal the external-scheme dialog below.
    {
        AdBlockManager *manager = AdBlockManager::instance();
        const int subsBefore = manager->subscriptions().count();
        ModalAnswer answer(QMessageBox::No, this);
        const QUrl abpUrl(QLatin1String("abp:subscribe?location=")
            + QString::fromUtf8(QUrl::fromLocalFile(
                QLatin1String("/nonexistent-wk17-list.txt")).toEncoded())
            + QLatin1String("&title=Wk17List"));
        page->load(abpUrl);
        QTRY_VERIFY2_WITH_TIMEOUT(answer.answered() >= 1,
                                  "abp: prompt never reached the app",
                                  15000);
        QTest::qWait(300);   // the prompt path + request failure settle
        QCOMPARE(manager->subscriptions().count(), subsBefore);
    }

    // — external scheme: refused in engine, the SEC02 consent prompt
    //   runs queued (ModalAnswer cancels); the page never moves.
    {
        ModalAnswer cancel(QMessageBox::Cancel, this);
        const QUrl external(
            QStringLiteral("mailto:wk17@example.invalid"));
        page->load(external);
        QTRY_VERIFY2_WITH_TIMEOUT(cancel.answered() >= 1,
                                  "external-scheme consent never ran",
                                  15000);
        QVERIFY(page->url() != external);
    }
#else
    QSKIP("webkit backend not linked in this build");
#endif
}

// WK18 — the declarative-blocking lane: the policy's rules document
// (a real AdBlockRustEngine::contentBlockerJson conversion) compiles
// through the shared WebKitUserContentFilterStore onto every page's
// UserContentManager; a named subresource must then never reach the
// wire, a cosmetic rule must hide its element, and the doc's meta
// channel reports converted/dropped/degraded counts.  A ruleset
// re-push exercises the setRequestPolicy refresh hook.
void tst_EngineSwitch::webkitContentBlocking()
{
#if defined(ARKTOS_WEBKIT_LINKED) && defined(ARKTOS_ADBLOCK_RUST)
    Engine::Backend *webkit =
        EngineRegistry::backendForId(QStringLiteral("webkit"));
    QVERIFY(webkit);
    QVERIFY2(webkit->pageUnavailableReason().isEmpty(),
             qPrintable(webkit->pageUnavailableReason()));
    // WK18's capability verdict — declarative blocking is the honest
    // name for the subresource lane; requestInterception stays on for
    // the WK17 navigation consult.
    QVERIFY(webkit->capabilities().declarativeBlocking);
    QVERIFY(webkit->capabilities().requestInterception);

    // The converter output drives the fixture — one pass exercises
    // block, cosmetic, block-cookies, degrade-to-block ($redirect)
    // and a true drop ($csp).
    const QByteArray rules =
        "/wk18-tracker.png\n"
        "||ck.invalid^$cookie\n"
        "##.wk18-ad\n"
        "||stub.invalid^$redirect=noop.js\n"
        "||csp.invalid^$csp=script-src 'none'\n";
    const QByteArray doc = AdBlockRustEngine::contentBlockerJson(rules);
    QVERIFY2(!doc.isEmpty(), "the converter returned no document");
    const QJsonObject meta = QJsonDocument::fromJson(doc).object()
        .value(QStringLiteral("meta")).toObject();
    const int emitted = meta.value(QStringLiteral("emitted")).toInt();
    const int dropped = meta.value(QStringLiteral("dropped")).toInt();
    const int degraded = meta.value(QStringLiteral("degraded")).toInt();
    // The verify contract — the converted/dropped counts are printed.
    qInfo("WK18 content-blocker doc: emitted=%d dropped=%d "
          "degraded=%d", emitted, dropped, degraded);
    for (const QJsonValue &v : QJsonDocument::fromJson(doc).object()
             .value(QStringLiteral("dropped")).toArray())
        qInfo("WK18 dropped: %s",
              qPrintable(v.toObject().value(QStringLiteral("why")).toString()
                         + " | " + v.toObject().value(QStringLiteral("rule")).toString()));
    QVERIFY2(emitted >= 4,
             "block + cookie + cosmetic + degraded rules expected");
    QCOMPARE(dropped, 1);    // the $csp rule
    QCOMPARE(degraded, 1);   // the $redirect rule became a block
    QVERIFY(QByteArray(doc).contains("block-cookies"));
    QVERIFY(QByteArray(doc).contains("css-display-none"));

    // The fixture policy consults nothing but carries the compiled
    // document — the profile pulls it through the WK18 surface.
    class FixturePolicy : public Engine::RequestPolicy
    {
    public:
        Engine::RequestDecision decide(
            const Engine::NavigationRequest &) override
        {
            return Engine::RequestDecision();
        }
        QByteArray contentBlockerJson() const override { return doc; }
        QByteArray doc;
    };
    FixturePolicy policy;
    policy.doc = doc;

    Engine::ProfileOptions options;
    options.offTheRecord = true;   // ephemeral session — no disk state
    Engine::Profile *profile = webkit->createProfile(options, this);
    QVERIFY(profile);
    auto *wkProfile = qobject_cast<WebkitProfile *>(profile);
    QVERIFY(wkProfile);
    profile->setRequestPolicy(&policy);
    // The store compile is async — ready lands when the filter (or an
    // honest empty) is applied to the profile's pages.
    QTRY_VERIFY2_WITH_TIMEOUT(wkProfile->contentBlockerReady(),
                              "content filter never compiled", 30000);
    QCOMPARE(wkProfile->contentBlockerEmitted(), emitted);
    QCOMPARE(wkProfile->contentBlockerDropped(), dropped);
    QCOMPARE(wkProfile->contentBlockerDegraded(), degraded);
    const int syncs = wkProfile->contentBlockerSyncs();

    // Loopback fixture: /wk18 serves the page; every request path is
    // recorded so the blocked subresource proves it never wired.
    QSet<QString> wirePaths;
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    auto pending =
        QSharedPointer<QHash<QTcpSocket *, QByteArray>>::create();
    connect(&server, &QTcpServer::newConnection, &server,
            [&server, &wirePaths, pending]() {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            socket->setParent(&server);
            connect(socket, &QTcpSocket::readyRead, socket,
                    [socket, &wirePaths, pending]() {
                QByteArray &request = (*pending)[socket];
                request += socket->readAll();
                if (!request.contains("\r\n\r\n"))
                    return;
                const int firstLineEnd = request.indexOf('\n');
                if (firstLineEnd > 0) {
                    const QByteArray line =
                        request.left(firstLineEnd).trimmed();
                    const QList<QByteArray> parts = line.split(' ');
                    if (parts.size() >= 2)
                        wirePaths.insert(
                            QString::fromUtf8(parts.at(1)));
                }
                QByteArray body;
                QByteArray type = "application/octet-stream";
                if (request.startsWith("GET /wk18 ")) {
                    type = "text/html";
                    body = "<html><head><title>Wk18</title></head>"
                           "<body><div class=\"wk18-ad\">AD</div>"
                           "<img src=\"/wk18-tracker.png\">"
                           "<img src=\"/wk18-ok.png\">"
                           "</body></html>";
                } else {
                    socket->write("HTTP/1.1 404 Not Found\r\n"
                                  "Content-Length: 0\r\n"
                                  "Connection: close\r\n\r\n");
                    socket->disconnect();
                    socket->disconnectFromHost();
                    pending->remove(socket);
                    return;
                }
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: " + type
                              + "\r\nContent-Length: "
                              + QByteArray::number(body.size())
                              + "\r\nConnection: close\r\n\r\n" + body);
                socket->disconnect();
                socket->disconnectFromHost();
                pending->remove(socket);
            });
        }
    });
    const QUrl fixtureUrl(QStringLiteral("http://127.0.0.1:%1/wk18")
                              .arg(server.serverPort()));

    EngineTab tab(webkit, profile);
    tab.resize(800, 600);
    tab.show();
    QVERIFY(tab.page());
    auto *page = qobject_cast<WebkitPage *>(tab.page());
    QVERIFY(page);

    QList<bool> loadResults;
    connect(page, &Engine::Page::loadFinished, page,
            [&](bool ok) { loadResults << ok; });
    auto waitLoadOk = [&](int timeout = 60000) {
        const int base = loadResults.size();
        for (int waited = 0; waited < timeout; waited += 100) {
            for (int i = base; i < loadResults.size(); ++i)
                if (loadResults.at(i))
                    return true;
            QTest::qWait(100);
        }
        return false;
    };

    tab.load(fixtureUrl);
    QVERIFY2(waitLoadOk(), "fixture load never finished");
    QTest::qWait(1500);   // settle any late subresource attempt

    // — block leg: the named subresource never reached the wire;
    //   the control resource proves the filter is not a blanket deny.
    QVERIFY2(!wirePaths.contains(QStringLiteral("/wk18-tracker.png")),
             "a content-blocked subresource reached the wire");
    QVERIFY2(wirePaths.contains(QStringLiteral("/wk18-ok.png")),
             "the control subresource should have loaded");

    // — cosmetic leg: the css-display-none rule hid the element.
    {
        std::shared_ptr<QVariant> result(new QVariant);
        std::shared_ptr<bool> done(new bool(false));
        page->runJavaScript(
            QStringLiteral(
                "getComputedStyle(document.querySelector('.wk18-ad'))"
                ".display"),
            [result, done](const QVariant &v) {
                *result = v;
                *done = true;
            });
        for (int waited = 0; !*done && waited < 15000; waited += 50)
            QTest::qWait(50);
        QVERIFY2(*done, "cosmetic eval never returned");
        QCOMPARE(result->toString(), QStringLiteral("none"));
    }

    // — refresh leg: a re-pushed ruleset recompiles and swaps the
    //   live filter (the setRequestPolicy refresh hook).
    const QByteArray doc2 = AdBlockRustEngine::contentBlockerJson(
        rules + "/wk18-late.png\n");
    QVERIFY2(!doc2.isEmpty(), "second conversion returned no doc");
    policy.doc = doc2;
    const int emitted2 = QJsonDocument::fromJson(doc2).object()
        .value(QStringLiteral("meta")).toObject()
        .value(QStringLiteral("emitted")).toInt();
    profile->setRequestPolicy(&policy);
    QTRY_COMPARE_WITH_TIMEOUT(wkProfile->contentBlockerSyncs(),
                              syncs + 1, 30000);
    QCOMPARE(wkProfile->contentBlockerEmitted(), emitted2);
#else
    QSKIP("needs the linked webkit backend and the adblock_rust "
          "converter in this build");
#endif
}

// WK19 — SEC06 on the WPE backend end-to-end: a real TLS rejection
// (the scratch selfsigned leaf) drives load-failed-with-tls-errors ->
// the neutral certificateError emission -> the nonce-bound
// arktos-cert-error: interstitial -> "proceed" installs a
// session-scoped exception on the network session and the refused
// page serves.  A forged nonce resolves nothing along the way.
void tst_EngineSwitch::webkitCertErrorInterstitial()
{
#ifdef ARKTOS_WEBKIT_LINKED
    QVERIFY2(QSslSocket::supportsSsl(), "TLS backend required");
    Engine::Backend *webkit =
        EngineRegistry::backendForId(QStringLiteral("webkit"));
    QVERIFY(webkit);
    QVERIFY2(webkit->pageUnavailableReason().isEmpty(),
             qPrintable(webkit->pageUnavailableReason()));

    TlsFixture server;
    QVERIFY(server.start(QStringLiteral("selfsigned"),
                         /*www=*/true));
    const QUrl serverUrl = tlsFixtureUrl(server);

    Engine::ProfileOptions options;
    options.offTheRecord = true;   // nothing may persist — SEC06
    Engine::Profile *profile = webkit->createProfile(options, this);
    QVERIFY(profile);

    EngineTab tab(webkit, profile);
    tab.resize(800, 600);
    tab.show();
    auto *page = qobject_cast<WebkitPage *>(tab.page());
    QVERIFY(page);

    // WPE 2.54 exposes no OS WebProcess pid — the honest contract.
    QCOMPARE(page->renderProcessId(), qint64(-1));

    QList<Engine::CertificateErrorInfo> certErrors;
    connect(page, &Engine::Page::certificateError, this,
            [&certErrors](const Engine::CertificateErrorInfo &e, bool *) {
                certErrors << e;
            });
    QList<bool> loadResults;
    int secureLoads = 0;   // a finished load OF the https page itself
    connect(page, &Engine::Page::loadFinished, page,
            [&](bool ok) {
                loadResults << ok;
                if (ok && page->url() == serverUrl)
                    ++secureLoads;
            });

    tab.load(serverUrl);

    // The neutral signal fires first, carrying the marshaled info.
    for (int w = 0; w < 30000 && certErrors.isEmpty(); w += 100)
        QTest::qWait(100);
    QVERIFY2(!certErrors.isEmpty(), "certificateError never fired");
    const Engine::CertificateErrorInfo info = certErrors.first();
    QCOMPARE(info.url.host(), QStringLiteral("127.0.0.1"));
    QVERIFY(info.isMainFrame);
    QVERIFY(info.overridable);
    QVERIFY2(!info.certificateChain.isEmpty(),
             "GTlsCertificate chain did not decode into QSslCertificate");
    QVERIFY(!info.description.isEmpty());

    // The dead navigation reports failure, the pending state is set,
    // then the interstitial commits its own success — all while the
    // refused page itself never finished a load.
    QTRY_VERIFY_WITH_TIMEOUT(loadResults.contains(false), 30000);
    QVERIFY(page->certErrorPending());
    QCOMPARE(page->certErrorUrl(), serverUrl);
    QTRY_VERIFY2_WITH_TIMEOUT(
        page->url().scheme() == QLatin1String("arktos-cert-error"),
        "the cert-error interstitial never committed", 30000);
    QTRY_VERIFY_WITH_TIMEOUT(loadResults.last() == true, 30000);
    QCOMPARE(secureLoads, 0);

    // A forged proceed nonce resolves nothing.
    const QString nonce = QUrlQuery(page->url())
        .queryItemValue(QStringLiteral("n"));
    QVERIFY(!nonce.isEmpty());
    tab.load(QUrl(QStringLiteral("arktos-cert-error:proceed?n=forged")));
    QTest::qWait(1500);
    QVERIFY(page->certErrorPending());
    QCOMPARE(secureLoads, 0);

    // The genuine proceed link installs the session exception and the
    // refused page now serves.
    tab.load(QUrl(QStringLiteral("arktos-cert-error:proceed?n=") + nonce));
    QTRY_VERIFY_WITH_TIMEOUT(page->url() == serverUrl, 30000);
    QTRY_VERIFY_WITH_TIMEOUT(secureLoads >= 1, 30000);
    QVERIFY(!page->certErrorPending());
    // The padlock record — a proceeded-past host can never wear green.
    QVERIFY(WebPage::certificateErrorOverridden(
        QStringLiteral("127.0.0.1")));
#else
    QSKIP("webkit backend not linked in this build");
#endif
}

// WK19 — right-click marshals the WebKitHitTestResult into
// ContextMenuInfo, emits the neutral signal and the view presents the
// WebKit item model as a Qt popup.
void tst_EngineSwitch::webkitContextMenu()
{
#ifdef ARKTOS_WEBKIT_LINKED
    Engine::Backend *webkit =
        EngineRegistry::backendForId(QStringLiteral("webkit"));
    QVERIFY(webkit);
    QVERIFY2(webkit->pageUnavailableReason().isEmpty(),
             qPrintable(webkit->pageUnavailableReason()));

    // A link stretched over the whole viewport — the hit-test lands
    // on it wherever the click falls.
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    connect(&server, &QTcpServer::newConnection, &server, [&server]() {
        QTcpSocket *socket = server.nextPendingConnection();
        socket->setParent(&server);
        connect(socket, &QTcpSocket::readyRead, socket, [socket]() {
            if (!socket->peek(4096).contains("\r\n\r\n"))
                return;
            socket->readAll();
            const QByteArray body = QByteArrayLiteral(
                "<html><body><a href=\"http://example.invalid/wk19\" "
                "style=\"position:fixed;inset:0;display:block\">"
                "wk19-link</a></body></html>");
            socket->write("HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n"
                          "Content-Length: "
                          + QByteArray::number(body.size())
                          + "\r\nConnection: close\r\n\r\n" + body);
            socket->disconnectFromHost();
        });
    });
    const QUrl fixtureUrl(QStringLiteral("http://127.0.0.1:%1/")
                              .arg(server.serverPort()));

    Engine::ProfileOptions options;
    options.offTheRecord = true;
    Engine::Profile *profile = webkit->createProfile(options, this);
    QVERIFY(profile);
    EngineTab tab(webkit, profile);
    tab.resize(800, 600);
    tab.show();
    auto *page = qobject_cast<WebkitPage *>(tab.page());
    auto *view = qobject_cast<WebkitView *>(tab.page()->view());
    QVERIFY(page && view);

    QSignalSpy loaded(page, &Engine::Page::loadFinished);
    tab.load(fixtureUrl);
    QTRY_VERIFY_WITH_TIMEOUT(!loaded.isEmpty()
            && loaded.last().at(0).toBool(), 60000);

    QSignalSpy menuSpy(page, &Engine::Page::contextMenuRequested);
    QTest::mouseClick(view, Qt::RightButton, Qt::NoModifier,
                      QPoint(120, 90));
    QTRY_VERIFY2_WITH_TIMEOUT(menuSpy.count() >= 1,
                              "context-menu never arrived", 15000);
    const Engine::ContextMenuInfo info =
        menuSpy.first().at(0).value<Engine::ContextMenuInfo>();
    QCOMPARE(info.linkUrl, QUrl(QStringLiteral("http://example.invalid/wk19")));
    QVERIFY(!info.linkText.isEmpty());
    QCOMPARE(info.position, QPoint(120, 90));
    QCOMPARE(info.pageUrl, fixtureUrl);

    // The WebKit-item popup was presented — dismiss it so it cannot
    // leak into the next leg.
    QTRY_VERIFY_WITH_TIMEOUT(
        QApplication::activePopupWidget() != nullptr, 10000);
    if (QWidget *popup = QApplication::activePopupWidget())
        popup->close();
    pumpDeletes();
#else
    QSKIP("webkit backend not linked in this build");
#endif
}

// WK19 — a page permission ask routes through WebPermissionManager's
// serialized prompt; the ModalAnswer denial resolves the WebKit
// request instead of leaving it hanging.
void tst_EngineSwitch::webkitPermissionRequest()
{
#ifdef ARKTOS_WEBKIT_LINKED
    Engine::Backend *webkit =
        EngineRegistry::backendForId(QStringLiteral("webkit"));
    QVERIFY(webkit);
    QVERIFY2(webkit->pageUnavailableReason().isEmpty(),
             qPrintable(webkit->pageUnavailableReason()));

    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    connect(&server, &QTcpServer::newConnection, &server, [&server]() {
        QTcpSocket *socket = server.nextPendingConnection();
        socket->setParent(&server);
        connect(socket, &QTcpSocket::readyRead, socket, [socket]() {
            if (!socket->peek(4096).contains("\r\n\r\n"))
                return;
            socket->readAll();
            const QByteArray body =
                QByteArrayLiteral("<html><body>wk19-perm</body></html>");
            socket->write("HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n"
                          "Content-Length: "
                          + QByteArray::number(body.size())
                          + "\r\nConnection: close\r\n\r\n" + body);
            socket->disconnectFromHost();
        });
    });

    Engine::ProfileOptions options;
    options.offTheRecord = true;
    Engine::Profile *profile = webkit->createProfile(options, this);
    QVERIFY(profile);
    EngineTab tab(webkit, profile);
    tab.resize(800, 600);
    tab.show();
    auto *page = qobject_cast<WebkitPage *>(tab.page());
    QVERIFY(page);

    QSignalSpy loaded(page, &Engine::Page::loadFinished);
    tab.load(QUrl(QStringLiteral("http://127.0.0.1:%1/")
                      .arg(server.serverPort())));
    QTRY_VERIFY_WITH_TIMEOUT(!loaded.isEmpty()
            && loaded.last().at(0).toBool(), 60000);

    ModalAnswer answerer(QMessageBox::No, this);   // auto-deny prompts
    QString fired;
    bool jsDone = false;
    page->runJavaScript(QStringLiteral(
        "(function(){"
        " var r='none';"
        " try { if (navigator.geolocation) {"
        "   navigator.geolocation.getCurrentPosition("
        "     function(){}, function(){}); r='geo'; } } catch(e) {}"
        " try { if (window.Notification && Notification.requestPermission) {"
        "   Notification.requestPermission(function(){});"
        "   r = r === 'none' ? 'notif' : r + '+notif'; } } catch(e) {}"
        " return r; })()"),
        [&fired, &jsDone](const QVariant &v) {
            fired = v.toString();
            jsDone = true;
        });
    for (int waited = 0; !jsDone && waited < 15000; waited += 50)
        QTest::qWait(50);
    QVERIFY2(jsDone, "permission probe eval never returned");

    // At least one mappable request must reach the broker and get
    // answered — a request that only ever hangs the page would be a
    // regression of the SEC05 contract.
    QTRY_VERIFY2_WITH_TIMEOUT(
        page->permissionRequestsSeen() >= 1,
        qPrintable(QStringLiteral("no permission request arrived "
                                  "(js: %1)").arg(fired)), 20000);
    QTRY_VERIFY_WITH_TIMEOUT(answerer.answered() >= 1, 20000);
    QCOMPARE(page->permissionRequestsGranted(), 0);
#else
    QSKIP("webkit backend not linked in this build");
#endif
}

// WK19 — the download channel end-to-end: a Content-Disposition
// attachment gets the RESPONSE decide-policy "download" verdict and
// surfaces as Engine::DownloadRequest through the profile; the same
// channel serves Page::download().  Accept inside the emission is the
// synchronous decide-destination answer.
void tst_EngineSwitch::webkitDownload()
{
#ifdef ARKTOS_WEBKIT_LINKED
    Engine::Backend *webkit =
        EngineRegistry::backendForId(QStringLiteral("webkit"));
    QVERIFY(webkit);
    QVERIFY2(webkit->pageUnavailableReason().isEmpty(),
             qPrintable(webkit->pageUnavailableReason()));

    const QByteArray payloadA("wk19-download-payload");
    const QByteArray payloadB("wk19-uri-payload");
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    connect(&server, &QTcpServer::newConnection, &server,
            [&server, payloadA, payloadB]() {
        QTcpSocket *socket = server.nextPendingConnection();
        socket->setParent(&server);
        connect(socket, &QTcpSocket::readyRead, socket,
                [socket, payloadA, payloadB]() {
            if (!socket->peek(4096).contains("\r\n\r\n"))
                return;
            const QByteArray request = socket->readAll();
            QByteArray head;
            QByteArray body;
            if (request.startsWith("GET /dl ")) {
                body = payloadA;
                head = "HTTP/1.0 200 OK\r\n"
                       "Content-Type: application/octet-stream\r\n"
                       "Content-Disposition: attachment; "
                       "filename=\"wk19.bin\"\r\n";
            } else {
                body = payloadB;
                head = "HTTP/1.0 200 OK\r\n"
                       "Content-Type: application/octet-stream\r\n";
            }
            socket->write(head + "Content-Length: "
                          + QByteArray::number(body.size())
                          + "\r\nConnection: close\r\n\r\n" + body);
            socket->disconnectFromHost();
        });
    });
    const QUrl base(QStringLiteral("http://127.0.0.1:%1")
                        .arg(server.serverPort()));

    Engine::ProfileOptions options;
    options.offTheRecord = true;
    Engine::Profile *profile = webkit->createProfile(options, this);
    QVERIFY(profile);
    EngineTab tab(webkit, profile);
    tab.resize(800, 600);
    tab.show();
    auto *page = qobject_cast<WebkitPage *>(tab.page());
    QVERIFY(page);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    int handoffs = 0;
    QString acceptedName, acceptedMime;
    qint64 acceptedTotal = -1;
    connect(profile, &Engine::Profile::downloadRequested, this,
            [&](Engine::DownloadRequest *request) {
                ++handoffs;
                if (handoffs == 1) {
                    acceptedName = request->suggestedFileName();
                    acceptedMime = request->mimeType();
                    acceptedTotal = request->totalBytes();
                }
                request->accept(dir.filePath(
                    handoffs == 1 ? QStringLiteral("one.bin")
                                  : QStringLiteral("two.bin")));
            });

    // Leg 1 — an attachment response becomes a download.
    tab.load(QUrl(base.toString() + QStringLiteral("/dl")));
    QTRY_VERIFY2_WITH_TIMEOUT(QFile::exists(dir.filePath(
                                  QStringLiteral("one.bin"))),
                              "attachment never landed on disk", 30000);
    {
        QFile file(dir.filePath(QStringLiteral("one.bin")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), payloadA);
    }
    QCOMPARE(handoffs, 1);
    QCOMPARE(acceptedName, QStringLiteral("wk19.bin"));
    QCOMPARE(acceptedMime, QStringLiteral("application/octet-stream"));
    QCOMPARE(acceptedTotal, qint64(payloadA.size()));

    // Leg 2 — Page::download() drives the same channel.
    page->download(QUrl(base.toString() + QStringLiteral("/dl2")));
    QTRY_VERIFY2_WITH_TIMEOUT(QFile::exists(dir.filePath(
                                  QStringLiteral("two.bin"))),
                              "download() never landed on disk", 30000);
    {
        QFile file(dir.filePath(QStringLiteral("two.bin")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), payloadB);
    }
    QCOMPARE(handoffs, 2);
#else
    QSKIP("webkit backend not linked in this build");
#endif
}

// WK20 — the GL import path end-to-end: a real frame must arrive
// through dmabuf -> EGLImage -> GL texture -> FBO readback with the
// fixture's witness colors intact (that pins the channel swizzle and
// the top-down orientation of the imported texture, not merely that
// a path ran).  Hosts without a usable EGL display get an honest
// skip — the raster fallback covered their frames.
void tst_EngineSwitch::webkitGlPath()
{
#ifdef ARKTOS_WEBKIT_LINKED
    Engine::Backend *webkit =
        EngineRegistry::backendForId(QStringLiteral("webkit"));
    QVERIFY(webkit);
    QVERIFY2(webkit->pageUnavailableReason().isEmpty(),
             qPrintable(webkit->pageUnavailableReason()));

    Engine::ProfileOptions options;
    options.offTheRecord = true;
    Engine::Profile *profile = webkit->createProfile(options, this);
    QVERIFY(profile);

    EngineTab tab(webkit, profile);
    tab.resize(800, 600);
    tab.show();
    QVERIFY(tab.page());
    auto *view = qobject_cast<WebkitView *>(tab.page()->view());
    QVERIFY(view);

    const QString fixturePath = QDir::temp().filePath(
        QStringLiteral("arktos-wk20-fixture.html"));
    {
        QFile fixture(fixturePath);
        QVERIFY(fixture.open(QIODevice::WriteOnly));
        fixture.write("<html><head><title>Wk20Gl</title></head>"
                      "<body style='margin:0;background:#eef4ff'>"
                      "<div style='width:400px;height:120px;"
                      "background:#123a6f;color:#22aa77'>"
                      "gl frame</div>"
                      "<div style='width:320px;height:160px;margin:40px;"
                      "background:#22aa77'></div>"
                      "</body></html>");
    }

    QSignalSpy finishedSpy(tab.page(), &Engine::Page::loadFinished);
    tab.load(QUrl::fromLocalFile(fixturePath));
    QTRY_VERIFY2_WITH_TIMEOUT(finishedSpy.count() >= 1,
                              "loadFinished never arrived", 60000);
    QVERIFY2(finishedSpy.first().first().toBool(),
             "fixture load reported failure");
    QTRY_VERIFY_WITH_TIMEOUT(view->frameCount() >= 1, 30000);

    // The first GL-imported frame is consumed by channel-order
    // calibration (presented via the raster copy) — force a new
    // buffer so a calibrated frame lands through adoptGlFrame.
    tab.resize(760, 540);
    QElapsedTimer glWait;
    glWait.start();
    while (view->glFrameCount() < 1 && glWait.elapsed() < 30000)
        QTest::qWait(50);

    if (view->glFrameCount() < 1)
        QSKIP(qPrintable(QStringLiteral(
            "GL frame path unavailable here (glImportFails=%1) — "
            "raster covered the frames").arg(view->glImportFails())));

    // Same witness gate as the raster leg — the GL path must deliver
    // identical pixels (channel swizzle + top-down orientation right).
    int distinctColors = 0;
    auto colorsReady = [&]() -> bool {
        const QImage shot = view->grab().toImage()
                .convertToFormat(QImage::Format_ARGB32);
        if (shot.isNull())
            return false;
        QSet<QRgb> colors;
        for (int y = 0; y < shot.height(); ++y) {
            const QRgb *row =
                reinterpret_cast<const QRgb *>(shot.constScanLine(y));
            for (int x = 0; x < shot.width(); ++x)
                colors.insert(row[x] | 0xff000000u);
        }
        distinctColors = colors.count();
        return colors.contains(QRgb(0xff123a6f))
            && colors.contains(QRgb(0xffeef4ff))
            && colors.contains(QRgb(0xff22aa77))
            && colors.count() >= 3;
    };
    QTRY_VERIFY2_WITH_TIMEOUT(colorsReady(),
             qPrintable(QStringLiteral("GL frames lack fixture colors — "
                                       "%1 distinct").arg(distinctColors)),
             30000);
    QCOMPARE(view->glImportFails(), 0);
#else
    QSKIP("webkit backend not linked in this build");
#endif
}

// WK20 — the raster fallback contract: with ARKTOS_WEBKIT_GL=0 the
// same fixture still paints frames through import_to_pixels and no GL
// attempt is counted.
void tst_EngineSwitch::webkitGlRasterFallback()
{
#ifdef ARKTOS_WEBKIT_LINKED
    Engine::Backend *webkit =
        EngineRegistry::backendForId(QStringLiteral("webkit"));
    QVERIFY(webkit);
    QVERIFY2(webkit->pageUnavailableReason().isEmpty(),
             qPrintable(webkit->pageUnavailableReason()));

    qputenv("ARKTOS_WEBKIT_GL", "0");

    Engine::ProfileOptions options;
    options.offTheRecord = true;
    Engine::Profile *profile = webkit->createProfile(options, this);
    QVERIFY(profile);

    EngineTab tab(webkit, profile);
    tab.resize(800, 600);
    tab.show();
    QVERIFY(tab.page());
    auto *view = qobject_cast<WebkitView *>(tab.page()->view());
    QVERIFY(view);

    const QString fixturePath = QDir::temp().filePath(
        QStringLiteral("arktos-wk20-fixture.html"));
    {
        QFile fixture(fixturePath);
        QVERIFY(fixture.open(QIODevice::WriteOnly));
        fixture.write("<html><head><title>Wk20Raster</title></head>"
                      "<body style='margin:0;background:#eef4ff'>"
                      "<div style='width:400px;height:120px;"
                      "background:#123a6f'>raster</div>"
                      "</body></html>");
    }

    QSignalSpy finishedSpy(tab.page(), &Engine::Page::loadFinished);
    tab.load(QUrl::fromLocalFile(fixturePath));
    QElapsedTimer wait;
    wait.start();
    while (finishedSpy.isEmpty() && wait.elapsed() < 60000)
        QTest::qWait(50);
    QVERIFY2(!finishedSpy.isEmpty(), "loadFinished never arrived");
    QVERIFY2(finishedSpy.first().first().toBool(),
             "fixture load reported failure");
    QTRY_VERIFY_WITH_TIMEOUT(view->frameCount() >= 1, 30000);
    // Snapshot under the pin — a post-restore frame may legitimately
    // take the GL branch on this view.
    const int glFrames = view->glFrameCount();
    const int glFails = view->glImportFails();
    qunsetenv("ARKTOS_WEBKIT_GL");
    QCOMPARE(glFrames, 0);
    QCOMPARE(glFails, 0);
#else
    QSKIP("webkit backend not linked in this build");
#endif
}

// MRG08 — webkit render churn: unlike engineTabChurn (which mints and
// drops the tab without navigating), each cycle here loads a real
// fixture and waits for a rendered frame before teardown — the WPE
// buffer/WebProcess frame path is the backend's heaviest allocation
// surface and only a load drives it.
void tst_EngineSwitch::webkitRenderChurn()
{
#ifdef ARKTOS_WEBKIT_LINKED
    Engine::Backend *webkit =
        EngineRegistry::backendForId(QStringLiteral("webkit"));
    if (!webkit || !webkit->pageUnavailableReason().isEmpty())
        QSKIP("webkit backend unavailable in this build");

    const QString fixturePath = QDir::temp().filePath(
        QStringLiteral("arktos-mrg08-fixture.html"));
    {
        QFile fixture(fixturePath);
        QVERIFY(fixture.open(QIODevice::WriteOnly));
        fixture.write("<html><head><title>Mrg08Churn</title></head>"
                      "<body style='background:#123a6f'>churn</body>"
                      "</html>");
    }

    for (int cycle = 0; cycle < 3; ++cycle) {
        Engine::ProfileOptions options;
        options.offTheRecord = true;
        Engine::Profile *profile = webkit->createProfile(options, this);
        QVERIFY(profile);
        EngineTab *tab = new EngineTab(webkit, profile);
        tab->resize(640, 480);
        QVERIFY2(tab->page(), "webkit page create failed");

        QSignalSpy finishedSpy(tab->page(), &Engine::Page::loadFinished);
        tab->load(QUrl::fromLocalFile(fixturePath));
        QTRY_VERIFY2_WITH_TIMEOUT(finishedSpy.count() >= 1,
                                  "loadFinished never arrived", 60000);
        QVERIFY2(finishedSpy.first().first().toBool(),
                 "fixture load reported failure");
        auto *view = qobject_cast<WebkitView *>(tab->page()->view());
        QVERIFY(view);
        QTRY_VERIFY_WITH_TIMEOUT(view->frameCount() >= 1, 30000);

        QPointer<EngineTab> tabGuard(tab);
        QPointer<Engine::Page> pageGuard(tab->page());
        QPointer<QWidget> viewGuard(view);
        delete tab;
        pumpDeletes();
        QVERIFY2(!tabGuard && !pageGuard && !viewGuard,
                 "webkit tab/page/view survived destruction");
    }
#else
    QSKIP("webkit backend not linked in this build");
#endif
}

// MRG08 — per-backend EngineTab churn at the raw engine interface:
// EngineRegistry::backends() runs the probes, so every live backend —
// webengine included — mints and destroys its profile/page/view trio
// through the EngineTab wrapper itself.  The TabWidget swap path can
// never build an EngineTab on the native engine (webengine→webengine
// swaps are refused), but the adapter pair EngineTab hosts is real
// allocation surface all the same.
void tst_EngineSwitch::engineBackendChurn()
{
    int exercised = 0;
    const QList<Engine::Backend *> backends = EngineRegistry::backends();
    for (Engine::Backend *backend : backends) {
        if (!backend || !backend->pageUnavailableReason().isEmpty())
            continue;
        ++exercised;
        for (int cycle = 0; cycle < 4; ++cycle) {
            Engine::ProfileOptions options;
            options.offTheRecord = true;
            Engine::Profile *profile =
                backend->createProfile(options, this);
            QVERIFY2(profile, qPrintable(backend->id()));
            EngineTab *tab = new EngineTab(backend, profile);
            QVERIFY2(tab->page(), qPrintable(backend->id()));
            QVERIFY2(tab->page()->view(), qPrintable(backend->id()));
            QPointer<EngineTab> tabGuard(tab);
            QPointer<Engine::Page> pageGuard(tab->page());
            QPointer<QWidget> viewGuard(tab->page()->view());
            delete tab;
            pumpDeletes();
            QVERIFY2(!tabGuard && !pageGuard && !viewGuard,
                     qPrintable(backend->id()));
        }
    }
    // webengine + fake are live on every flavor — anything less means
    // the registry probe itself is broken, not that backends skipped.
    QVERIFY2(exercised >= 2,
             qPrintable(QStringLiteral("only %1 live backends")
                            .arg(exercised)));
}

// MRG08 — ENG36's churn path specifically: several LIVE servo views at
// once on the shared engine, then staggered teardown.  Each ServoView
// owns a se_view_create instance with its own surfman
// SoftwareRenderingContext — concurrent mint + interleaved destroy is
// the densest EGL/surface churn the backend offers.
void tst_EngineSwitch::servoViewChurn()
{
    Engine::Backend *servo =
        EngineRegistry::backendForId(QStringLiteral("servo"));
    if (!servo || !servo->pageUnavailableReason().isEmpty())
        QSKIP("servo backend unavailable in this build");

    Engine::ProfileOptions options;
    options.offTheRecord = true;
    Engine::Profile *profile = servo->createProfile(options, this);
    QVERIFY(profile);

    // Three rounds of three concurrent live views, destroyed in
    // reverse order — siblings must stay usable while a view dies.
    for (int round = 0; round < 3; ++round) {
        QList<EngineTab *> tabs;
        for (int i = 0; i < 3; ++i) {
            EngineTab *tab = new EngineTab(servo, profile);
            tab->resize(320, 240);
            QVERIFY2(tab->page() && tab->page()->view(),
                     "servo view create failed");
            tabs.append(tab);
        }
        while (!tabs.isEmpty()) {
            EngineTab *tab = tabs.takeLast();
            QPointer<Engine::Page> pageGuard(tab->page());
            QPointer<QWidget> viewGuard(tab->page()->view());
            delete tab;
            pumpDeletes();
            QVERIFY2(!pageGuard && !viewGuard,
                     "servo page/view survived destruction");
            for (EngineTab *sibling : tabs)
                QVERIFY2(sibling->page(), "sibling died with a view");
        }
    }
    // Plus six plain sequential cycles — mint/destroy against the
    // already-live engine.
    for (int cycle = 0; cycle < 6; ++cycle) {
        EngineTab *tab = new EngineTab(servo, profile);
        QVERIFY2(tab->page(), "servo view create failed");
        QPointer<EngineTab> guard(tab);
        delete tab;
        pumpDeletes();
        QVERIFY2(!guard, "servo tab survived destruction");
    }
}

// MRG08 — regression for the MRG06-filed cross-engine poison: servo
// view teardown runs surfman's EGL teardown on the GUI thread, which
// left the thread's bound API on desktop OpenGL (a webkit GLES2
// context minted afterwards compiled 'precision'-less desktop shaders
// and degraded to raster) and, on the MRG06 build, eglTerminate'd the
// Mesa-deduped surfaceless display out from under a cached Ready
// pipe.  Sequence: prove GL works, hold the pipe cached, churn servo,
// prove a fresh webkit view still draws through GL.
void tst_EngineSwitch::webkitGlSurvivesServoChurn()
{
#ifdef ARKTOS_WEBKIT_LINKED
    Engine::Backend *webkit =
        EngineRegistry::backendForId(QStringLiteral("webkit"));
    Engine::Backend *servo =
        EngineRegistry::backendForId(QStringLiteral("servo"));
    if (!webkit || !webkit->pageUnavailableReason().isEmpty())
        QSKIP("webkit backend unavailable in this build");
    if (!servo || !servo->pageUnavailableReason().isEmpty())
        QSKIP("servo backend unavailable in this build");

    const QString fixturePath = QDir::temp().filePath(
        QStringLiteral("arktos-wk20-fixture.html"));
    {
        QFile fixture(fixturePath);
        QVERIFY(fixture.open(QIODevice::WriteOnly));
        fixture.write("<html><head><title>Mrg08Gl</title></head>"
                      "<body style='margin:0;background:#eef4ff'>"
                      "<div style='width:400px;height:120px;"
                      "background:#123a6f'>gl after servo</div>"
                      "</body></html>");
    }

    Engine::ProfileOptions options;
    options.offTheRecord = true;

    auto renderGlFrames = [&](int warmupResize) -> int {
        Engine::Profile *profile = webkit->createProfile(options, this);
        if (!profile)
            return -1;
        EngineTab tab(webkit, profile);
        tab.resize(800, 600);
        tab.show();
        if (!tab.page())
            return -1;
        auto *view = qobject_cast<WebkitView *>(tab.page()->view());
        if (!view)
            return -1;
        QSignalSpy finishedSpy(tab.page(), &Engine::Page::loadFinished);
        tab.load(QUrl::fromLocalFile(fixturePath));
        QElapsedTimer wait;
        wait.start();
        while (finishedSpy.isEmpty() && wait.elapsed() < 60000)
            QTest::qWait(50);
        if (finishedSpy.isEmpty()
                || !finishedSpy.first().first().toBool())
            return -1;
        // First GL-imported frame feeds channel-order calibration;
        // a resize forces another buffer so an adopted GL frame lands.
        if (warmupResize)
            tab.resize(760, 540);
        QElapsedTimer glWait;
        glWait.start();
        while (view->glFrameCount() < 1 && glWait.elapsed() < 30000)
            QTest::qWait(50);
        return view->glFrameCount();
    };

    const int before = renderGlFrames(1);
    if (before < 1)
        QSKIP("GL frame path unavailable here — raster hosts cannot "
              "regress");

    // The poison: three servo view create/destroy rounds on the GUI
    // thread while the webkit pipe sits cached-Ready.
    Engine::Profile *servoProfile = servo->createProfile(options, this);
    QVERIFY(servoProfile);
    for (int cycle = 0; cycle < 3; ++cycle) {
        EngineTab *tab = new EngineTab(servo, servoProfile);
        QVERIFY2(tab->page(), "servo view create failed");
        delete tab;
        pumpDeletes();
    }

    const int after = renderGlFrames(1);
    QVERIFY2(after >= 1, qPrintable(QStringLiteral(
        "webkit GL path dead after servo churn (before=%1 after=%2)")
            .arg(before).arg(after)));
#else
    QSKIP("webkit backend not linked in this build");
#endif
}

QTEST_MAIN(tst_EngineSwitch)
#include "tst_engineswitch.moc"
