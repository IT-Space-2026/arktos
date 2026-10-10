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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

// ENG04 — drives the Engine interface through a fake backend so the
// boundary is test-proven independent of Chromium, then checks the
// real QtWebEngine adapter forwards and translates faithfully.

#include <QtCore/QtCore>
#include <QtGui/QtGui>
#include <QtTest/QtTest>
#include <QtWebEngineWidgets>

#include <engineinterface.h>
#include <webenginebackend.h>
#include <webview.h>

#include "qtest_arora.h"

// --- fake backend ----------------------------------------------------

class FakePage : public Engine::Page
{
    Q_OBJECT

public:
    explicit FakePage(QObject *parent = nullptr) : Engine::Page(parent) {}

    void load(const QUrl &newUrl) override
    {
        m_url = newUrl;
        emit loadStarted();
        emit loadProgress(100);
        emit loadFinished(true);
    }
    void stop() override { ++stops; }
    void reload() override { ++reloads; }
    QUrl url() const override { return m_url; }

    bool canGoBack() const override { return m_canBack; }
    bool canGoForward() const override { return m_canForward; }
    void back() override { m_canForward = true; m_canBack = false; }
    void forward() override { m_canBack = true; m_canForward = false; }

    void setZoomFactor(qreal factor) override { m_zoom = factor; }
    qreal zoomFactor() const override { return m_zoom; }

    void findText(const QString &subString, Engine::FindFlags options) override
    {
        lastFind = subString;
        lastFindFlags = options;
        Engine::FindResult result;
        result.numberOfMatches = subString.isEmpty() ? 0 : 2;
        result.activeMatch = 1;
        emit findTextFinished(result);
    }

    void runJavaScript(const QString &source,
                       const std::function<void(const QVariant &)> &resultCallback
                           = std::function<void(const QVariant &)>()) override
    {
        lastScript = source;
        if (resultCallback)
            resultCallback(QVariant(QStringLiteral("fake-result")));
    }

    void setPageAttribute(const QString &name, bool on) override
    {
        attributes.insert(name, on);
    }

    void setLifecycleState(LifecycleState state) override { m_lifecycle = state; }
    LifecycleState lifecycleState() const override { return m_lifecycle; }

    qint64 renderProcessId() const override { return m_pid; }
    void setRenderProcessId(qint64 pid) { m_pid = pid; }

    Engine::Page *createWindow(Engine::WebWindowType type) override
    {
        lastWindowType = type;
        return windowResult;
    }

    QUrl m_url;
    qreal m_zoom = 1.0;
    bool m_canBack = false;
    bool m_canForward = false;
    LifecycleState m_lifecycle = LifecycleState::Active;
    qint64 m_pid = -1;
    QString lastFind;
    Engine::FindFlags lastFindFlags;
    QString lastScript;
    QHash<QString, bool> attributes;
    Engine::WebWindowType lastWindowType = Engine::WebWindowType::BrowserWindow;
    Engine::Page *windowResult = nullptr;
    int stops = 0;
    int reloads = 0;
};

class FakeProfile : public Engine::Profile
{
    Q_OBJECT

public:
    explicit FakeProfile(QObject *parent = nullptr) : Engine::Profile(parent) {}

    bool isOffTheRecord() const override { return m_otr; }
    QString storageName() const override { return m_name; }
    void setUserAgent(const QString &userAgent) override { m_ua = userAgent; }
    void setRequestPolicy(Engine::RequestPolicy *policy) override { m_policy = policy; }
    Engine::RequestPolicy *requestPolicy() const override { return m_policy; }
    void setCookieFilter(
            const std::function<bool(const Engine::CookieAttempt &)> &filter) override
    {
        m_filter = filter;
    }
    void insertScript(const Engine::Script &script) override
    {
        m_scripts.insert(script.name, script);
    }
    void removeScript(const QString &name) override { m_scripts.remove(name); }
    QList<Engine::Script> scripts() const override { return m_scripts.values(); }
    void clear(Engine::StorageAreas areas) override { m_cleared |= areas; }
    void setProfileAttribute(const QString &name, const QVariant &value) override
    {
        m_attributes.insert(name, value);
    }

    bool m_otr = false;
    QString m_name;
    QString m_ua;
    Engine::RequestPolicy *m_policy = nullptr;
    std::function<bool(const Engine::CookieAttempt &)> m_filter;
    QHash<QString, Engine::Script> m_scripts;
    Engine::StorageAreas m_cleared;
    QHash<QString, QVariant> m_attributes;
};

class FakeBackend : public Engine::Backend
{
    Q_OBJECT

public:
    explicit FakeBackend(QObject *parent = nullptr) : Engine::Backend(parent) {}

    QString id() const override { return QStringLiteral("fake"); }
    QString displayName() const override { return QStringLiteral("Fake Engine"); }
    Engine::Capabilities capabilities() const override
    {
        Engine::Capabilities caps;
        caps.scriptEvaluation = true;
        return caps;
    }
    bool initialize() override { return true; }
    Engine::Profile *createProfile(const Engine::ProfileOptions &options,
                                   QObject *parent = nullptr) override
    {
        auto *profile = new FakeProfile(parent);
        profile->m_otr = options.offTheRecord;
        profile->m_name = options.storageName;
        return profile;
    }
    Engine::Page *createPage(Engine::Profile *, QObject *parent = nullptr) override
    {
        return new FakePage(parent);
    }
    QWidget *createView(Engine::Page *, QWidget *parent = nullptr) override
    {
        return new QWidget(parent);
    }
};

// --- test ------------------------------------------------------------

class tst_EngineAdapter : public QObject
{
    Q_OBJECT

private slots:
    void fakeBackendShape();
    void fakePageNavigation();
    void fakePageZoomFindScript();
    void fakePageLifecycleAndPid();
    void fakeProfileSurface();
    void webEngineBackendShape();
    void webEnginePageForwarding();
    void webEngineProfileSurface();
};

void tst_EngineAdapter::fakeBackendShape()
{
    FakeBackend backend;
    QCOMPARE(backend.id(), QStringLiteral("fake"));
    QVERIFY(backend.initialize());
    QVERIFY(backend.capabilities().scriptEvaluation);
    QVERIFY(!backend.capabilities().requestInterception);

    Engine::Profile *profile =
        backend.createProfile(Engine::ProfileOptions(), &backend);
    QVERIFY(profile);
    Engine::Page *page = backend.createPage(profile, &backend);
    QVERIFY(page);
    QVERIFY(backend.createView(page));
}

void tst_EngineAdapter::fakePageNavigation()
{
    FakePage page;
    QSignalSpy started(&page, &Engine::Page::loadStarted);
    QSignalSpy progress(&page, &Engine::Page::loadProgress);
    QSignalSpy finished(&page, &Engine::Page::loadFinished);

    const QUrl url(QStringLiteral("https://example.org/"));
    page.load(url);
    QCOMPARE(page.url(), url);
    QCOMPARE(started.count(), 1);
    QCOMPARE(progress.count(), 1);
    QCOMPARE(finished.count(), 1);

    page.back();
    QVERIFY(!page.canGoBack());
    QVERIFY(page.canGoForward());
    page.forward();
    QVERIFY(page.canGoBack());
    QVERIFY(!page.canGoForward());
    page.stop();
    page.reload();
    QCOMPARE(page.stops, 1);
    QCOMPARE(page.reloads, 1);
}

void tst_EngineAdapter::fakePageZoomFindScript()
{
    FakePage page;

    page.setZoomFactor(1.5);
    QCOMPARE(page.zoomFactor(), 1.5);

    qRegisterMetaType<Engine::FindResult>();
    QSignalSpy findSpy(&page, &Engine::Page::findTextFinished);
    page.findText(QStringLiteral("needle"), Engine::FindBackward);
    QCOMPARE(page.lastFind, QStringLiteral("needle"));
    QVERIFY(page.lastFindFlags & Engine::FindBackward);
    QCOMPARE(findSpy.count(), 1);
    const Engine::FindResult result =
        findSpy.first().first().value<Engine::FindResult>();
    QCOMPARE(result.numberOfMatches, 2);
    QCOMPARE(result.activeMatch, 1);

    QVariant seen;
    page.runJavaScript(QStringLiteral("1+1"),
        [&seen](const QVariant &value) { seen = value; });
    QCOMPARE(page.lastScript, QStringLiteral("1+1"));
    QCOMPARE(seen.toString(), QStringLiteral("fake-result"));

    page.setPageAttribute(QStringLiteral("JavascriptEnabled"), false);
    QCOMPARE(page.attributes.value(QStringLiteral("JavascriptEnabled")), false);
}

void tst_EngineAdapter::fakePageLifecycleAndPid()
{
    FakePage page;
    QCOMPARE(page.lifecycleState(), Engine::Page::LifecycleState::Active);
    page.setLifecycleState(Engine::Page::LifecycleState::Discarded);
    QCOMPARE(page.lifecycleState(), Engine::Page::LifecycleState::Discarded);

    // Passthrough shape: -1 means "engine has no per-page process".
    QCOMPARE(page.renderProcessId(), qint64(-1));
    page.setRenderProcessId(4242);
    QCOMPARE(page.renderProcessId(), qint64(4242));

    FakePage child;
    page.windowResult = &child;
    QCOMPARE(page.createWindow(Engine::WebWindowType::BrowserTab),
             static_cast<Engine::Page*>(&child));
    QCOMPARE(page.lastWindowType, Engine::WebWindowType::BrowserTab);
}

void tst_EngineAdapter::fakeProfileSurface()
{
    FakeProfile profile;
    profile.insertScript(Engine::Script{
        QStringLiteral("s"), QStringLiteral("code"),
        Engine::InjectionPoint::DocumentReady, 0, true });
    QCOMPARE(profile.scripts().count(), 1);
    profile.removeScript(QStringLiteral("s"));
    QVERIFY(profile.scripts().isEmpty());

    profile.clear(Engine::HttpCacheArea | Engine::CookiesArea);
    QVERIFY(profile.m_cleared & Engine::HttpCacheArea);
    QVERIFY(profile.m_cleared & Engine::CookiesArea);
    QVERIFY(!(profile.m_cleared & Engine::VisitedLinksArea));
}

void tst_EngineAdapter::webEngineBackendShape()
{
    Engine::Backend *backend = WebEngineBackend::instance();
    QCOMPARE(backend->id(), QStringLiteral("webengine"));
    QVERIFY(backend->displayName().contains(QLatin1String("QtWebEngine")));
    QVERIFY(backend->initialize());
    QVERIFY(backend->capabilities().requestInterception);
    QVERIFY(backend->capabilities().downloads);
}

void tst_EngineAdapter::webEnginePageForwarding()
{
    // The real adapter wraps the app's own WebPage — every call must
    // land on the underlying QWebEnginePage 1:1.
    WebView view;
    Engine::Page *enginePage = view.enginePage();
    QVERIFY(enginePage);
    QCOMPARE(enginePage->url(), view.page()->url());

    enginePage->setZoomFactor(1.25);
    QCOMPARE(enginePage->zoomFactor(), 1.25);
    QCOMPARE(view.page()->zoomFactor(), 1.25);

    QCOMPARE(enginePage->renderProcessId(), view.page()->renderProcessPid());

    // Signals re-emit on the interface: a real load drives
    // loadStarted/loadFinished through the adapter.
    QSignalSpy loadSpy(enginePage, &Engine::Page::loadFinished);
    enginePage->load(QUrl(QStringLiteral("data:text/html,<title>ENG04</title>")));
    QTRY_VERIFY_WITH_TIMEOUT(loadSpy.count() >= 1, 15000);
    QCOMPARE(enginePage->url().scheme(), QStringLiteral("data"));

    // The lifecycle mapping is value-identical on this backend.
    QCOMPARE(enginePage->lifecycleState(),
             static_cast<Engine::Page::LifecycleState>(
                 view.page()->lifecycleState()));
}

void tst_EngineAdapter::webEngineProfileSurface()
{
    // Profile adapter over the app's real profile: script collection
    // round-trips through the engine and attributes stay unknown-safe.
    Engine::Profile *profile = WebEngineBackend::instance()->createProfile(
        Engine::ProfileOptions(), this);
    QVERIFY(profile);
    // Default options wrap the shared default profile — the flag is a
    // passthrough, not a policy.
    QCOMPARE(profile->isOffTheRecord(),
             QWebEngineProfile::defaultProfile()->isOffTheRecord());

    Engine::ProfileOptions otr;
    otr.offTheRecord = true;
    Engine::Profile *otrProfile = WebEngineBackend::instance()->createProfile(
        otr, this);
    QVERIFY(otrProfile->isOffTheRecord());

    profile->insertScript(Engine::Script{
        QStringLiteral("eng04-test"), QStringLiteral("void 0"),
        Engine::InjectionPoint::DocumentReady, 0, true });
    const QList<Engine::Script> scripts = profile->scripts();
    bool found = false;
    for (const Engine::Script &script : scripts) {
        if (script.name == QLatin1String("eng04-test")) {
            found = true;
            QCOMPARE(script.injectionPoint, Engine::InjectionPoint::DocumentReady);
            QVERIFY(script.runsOnSubFrames);
        }
    }
    QVERIFY(found);
    profile->removeScript(QStringLiteral("eng04-test"));

    // Unknown attribute names are contract-ignored, not fatal.
    Engine::Page *page =
        WebEngineBackend::instance()->createPage(profile, this);
    page->setPageAttribute(QStringLiteral("NotARealAttribute"), true);
    page->setPageAttribute(QStringLiteral("JavascriptEnabled"), true);
}

QTEST_MAIN(tst_EngineAdapter)
#include "tst_engineadapter.moc"
