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

// --- fake backend ----------------------------------------------------

class SwitchFakePage : public Engine::Page
{
    Q_OBJECT

public:
    explicit SwitchFakePage(QObject *parent = nullptr)
        : Engine::Page(parent) {}

    void load(const QUrl &newUrl) override
    {
        m_url = newUrl;
        ++loads;
        emit urlChanged(newUrl);
        emit loadStarted();
        emit loadProgress(100);
        emit loadFinished(true);
    }
    void stop() override {}
    void reload() override { ++reloads; }
    QUrl url() const override { return m_url; }

    bool canGoBack() const override { return false; }
    bool canGoForward() const override { return false; }
    void back() override {}
    void forward() override {}

    int historyCount() const override { return m_url.isEmpty() ? 0 : 1; }
    int currentHistoryIndex() const override { return m_url.isEmpty() ? -1 : 0; }
    QList<Engine::HistoryEntry> historyItems() const override
    {
        if (m_url.isEmpty())
            return {};
        Engine::HistoryEntry entry;
        entry.url = m_url;
        entry.index = 0;
        return {entry};
    }
    QList<Engine::HistoryEntry> backItems(int) const override { return {}; }
    QList<Engine::HistoryEntry> forwardItems(int) const override { return {}; }
    void goToHistoryEntry(const Engine::HistoryEntry &) override {}

    void setZoomFactor(qreal factor) override { m_zoom = factor; }
    qreal zoomFactor() const override { return m_zoom; }

    void findText(const QString &, Engine::FindFlags) override
    {
        emit findTextFinished(Engine::FindResult());
    }

    void runJavaScript(const QString &,
                       const std::function<void(const QVariant &)> &resultCallback
                           = std::function<void(const QVariant &)>()) override
    {
        if (resultCallback)
            resultCallback(QVariant());
    }
    void runJavaScriptLifted(const QString &source,
                       const std::function<void(const QVariant &)> &resultCallback
                           = std::function<void(const QVariant &)>()) override
    {
        runJavaScript(source, resultCallback);
    }
    void toHtml(const std::function<void(const QString &)> &resultCallback) override
    {
        if (resultCallback)
            resultCallback(QString());
    }

    QAction *action(Engine::StandardAction) override { return nullptr; }

    bool isLoading() const override { return false; }
    bool recentlyAudible() const override { return false; }
    bool isOffTheRecord() const override { return false; }

    void setPageAttribute(const QString &, bool) override {}
    void setLifecycleState(LifecycleState state) override { m_lifecycle = state; }
    LifecycleState lifecycleState() const override { return m_lifecycle; }
    qint64 renderProcessId() const override { return -1; }
    Engine::Page *createWindow(Engine::WebWindowType) override { return nullptr; }

    void insertScript(const Engine::Script &) override {}
    void removeScript(const QString &) override {}
    QList<Engine::Script> scripts() const override { return {}; }

    void download(const QUrl &) override {}
    QWidget *view() const override { return m_view; }

    QUrl m_url;
    qreal m_zoom = 1.0;
    int loads = 0;
    int reloads = 0;
    LifecycleState m_lifecycle = LifecycleState::Active;
    QWidget *m_view = nullptr;
};

class SwitchFakeProfile : public Engine::Profile
{
    Q_OBJECT

public:
    explicit SwitchFakeProfile(QObject *parent = nullptr)
        : Engine::Profile(parent) {}

    bool isOffTheRecord() const override { return false; }
    QString storageName() const override { return QStringLiteral("fake"); }
    void setUserAgent(const QString &) override {}
    void setRequestPolicy(Engine::RequestPolicy *) override {}
    Engine::RequestPolicy *requestPolicy() const override { return nullptr; }
    void setCookieFilter(
            const std::function<bool(const Engine::CookieAttempt &)> &) override {}
    void insertScript(const Engine::Script &) override {}
    void removeScript(const QString &) override {}
    QList<Engine::Script> scripts() const override { return {}; }
    void clear(Engine::StorageAreas) override {}
    void setProfileAttribute(const QString &, const QVariant &) override {}
};

class SwitchFakeBackend : public Engine::Backend
{
    Q_OBJECT

public:
    explicit SwitchFakeBackend(QObject *parent = nullptr)
        : Engine::Backend(parent) {}

    QString id() const override { return QStringLiteral("fake"); }
    QString displayName() const override { return QStringLiteral("Fake Engine"); }
    Engine::Capabilities capabilities() const override
    {
        Engine::Capabilities caps;
        caps.downloads = false;
        return caps;
    }
    bool initialize() override { return true; }
    Engine::Profile *createProfile(const Engine::ProfileOptions &,
                                   QObject *parent = nullptr) override
    {
        return new SwitchFakeProfile(parent);
    }
    Engine::Page *createPage(Engine::Profile *,
                             QObject *parent = nullptr) override
    {
        auto *page = new SwitchFakePage(parent);
        pages.append(page);
        return page;
    }
    QWidget *createView(Engine::Page *page, QWidget *parent = nullptr) override
    {
        auto *view = new QWidget(parent);
        if (auto *fakePage = qobject_cast<SwitchFakePage*>(page))
            fakePage->m_view = view;
        return view;
    }

    QList<QPointer<SwitchFakePage>> pages;
};

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

private:
    SwitchFakeBackend *m_fake = nullptr;
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
    m_fake = new SwitchFakeBackend(this);
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
    SwitchFakePage *fakePage =
        qobject_cast<SwitchFakePage*>(widget.engineTab(0)->page());
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

QTEST_MAIN(tst_EngineSwitch)
#include "tst_engineswitch.moc"
