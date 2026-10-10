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

    int historyCount() const override { return m_entries.count(); }
    int currentHistoryIndex() const override { return m_current; }
    QList<Engine::HistoryEntry> historyItems() const override
    {
        return m_entries;
    }
    QList<Engine::HistoryEntry> backItems(int maxItems) const override
    {
        return m_entries.mid(qMax(0, m_current - maxItems),
                             qMin(maxItems, m_current));
    }
    QList<Engine::HistoryEntry> forwardItems(int maxItems) const override
    {
        return m_entries.mid(m_current + 1, maxItems);
    }
    void goToHistoryEntry(const Engine::HistoryEntry &entry) override
    {
        lastGoTo = entry.index;
    }

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

    void runJavaScriptLifted(const QString &source,
                       const std::function<void(const QVariant &)> &resultCallback
                           = std::function<void(const QVariant &)>()) override
    {
        ++liftedCalls;
        lastScript = source;
        if (resultCallback)
            resultCallback(QVariant(QStringLiteral("fake-lifted")));
    }

    void toHtml(const std::function<void(const QString &)> &resultCallback) override
    {
        if (resultCallback)
            resultCallback(m_markup);
    }

    QAction *action(Engine::StandardAction which) override
    {
        return actions.value(which);
    }

    bool isLoading() const override { return m_loading; }
    bool recentlyAudible() const override { return m_audible; }
    bool isOffTheRecord() const override { return m_otr; }

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

    void insertScript(const Engine::Script &script) override
    {
        m_scripts.insert(script.name, script);
    }
    void removeScript(const QString &name) override { m_scripts.remove(name); }
    QList<Engine::Script> scripts() const override { return m_scripts.values(); }

    void download(const QUrl &url) override { lastDownload = url; }
    QWidget *view() const override { return m_view; }

    QUrl m_url;
    qreal m_zoom = 1.0;
    bool m_canBack = false;
    bool m_canForward = false;
    bool m_loading = false;
    bool m_audible = false;
    bool m_otr = false;
    QHash<Engine::StandardAction, QAction*> actions;
    QList<Engine::HistoryEntry> m_entries;
    int m_current = -1;
    int lastGoTo = -1;
    LifecycleState m_lifecycle = LifecycleState::Active;
    qint64 m_pid = -1;
    QString lastFind;
    Engine::FindFlags lastFindFlags;
    QString lastScript;
    int liftedCalls = 0;
    QString m_markup;
    QHash<QString, bool> attributes;
    Engine::WebWindowType lastWindowType = Engine::WebWindowType::BrowserWindow;
    Engine::Page *windowResult = nullptr;
    int stops = 0;
    int reloads = 0;
    QHash<QString, Engine::Script> m_scripts;
    QUrl lastDownload;
    QWidget *m_view = nullptr;
};

class FakeDownloadRequest : public Engine::DownloadRequest
{
    Q_OBJECT

public:
    explicit FakeDownloadRequest(QObject *parent = nullptr)
        : Engine::DownloadRequest(parent) {}

    QUrl url() const override { return m_url; }
    QString suggestedFileName() const override { return m_name; }
    QString mimeType() const override { return m_mime; }
    void accept(const QString &filePath) override
    {
        acceptedPath = filePath;
        setState(State::InProgress);
    }
    void cancel() override { setState(State::Cancelled); }
    void pause() override { ++pauses; }
    void resume() override { ++resumes; }
    qint64 receivedBytes() const override { return m_received; }
    qint64 totalBytes() const override { return m_total; }
    State state() const override { return m_state; }
    bool isFinished() const override
    {
        return m_state == State::Completed || m_state == State::Cancelled;
    }
    QString interruptReasonString() const override { return m_interrupt; }
    void setDownloadDirectory(const QString &directory) override
    {
        m_dir = directory;
    }
    void setDownloadFileName(const QString &fileName) override
    {
        m_fileName = fileName;
    }
    QString downloadDirectory() const override { return m_dir; }
    QString downloadFileName() const override { return m_fileName; }
    Engine::Page *page() const override { return m_page; }

    void setState(State state)
    {
        m_state = state;
        emit stateChanged(state);
    }
    void bump(qint64 received, qint64 total)
    {
        m_received = received;
        emit receivedBytesChanged();
        if (total != m_total) {
            m_total = total;
            emit totalBytesChanged();
        }
    }

    QUrl m_url;
    QString m_name;
    QString m_mime;
    State m_state = State::Requested;
    qint64 m_received = 0;
    qint64 m_total = -1;
    QString m_interrupt;
    QString m_dir;
    QString m_fileName;
    QString acceptedPath;
    int pauses = 0;
    int resumes = 0;
    Engine::Page *m_page = nullptr;
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
    void fakeDownloadSurface();
    void webEngineBackendShape();
    void webEnginePageForwarding();
    void webEngineLiftedScript();
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

    // Standard-action + probe surface: unmapped actions come back
    // null (chrome disables the entry), probes answer false.
    QVERIFY(!page.action(Engine::StandardAction::Back));
    QAction reloadAction;
    page.actions.insert(Engine::StandardAction::Reload, &reloadAction);
    QCOMPARE(page.action(Engine::StandardAction::Reload), &reloadAction);
    QVERIFY(!page.isLoading());
    QVERIFY(!page.recentlyAudible());
    QVERIFY(!page.isOffTheRecord());

    // History surface: entries carry their stack index for goTo.
    page.m_entries = {
        Engine::HistoryEntry{ QUrl(QStringLiteral("https://a/")),
                              QStringLiteral("a"), 0 },
        Engine::HistoryEntry{ QUrl(QStringLiteral("https://b/")),
                              QStringLiteral("b"), 1 },
        Engine::HistoryEntry{ QUrl(QStringLiteral("https://c/")),
                              QStringLiteral("c"), 2 } };
    page.m_current = 1;
    QCOMPARE(page.historyCount(), 3);
    QCOMPARE(page.currentHistoryIndex(), 1);
    QCOMPARE(page.backItems(5).count(), 1);
    QCOMPARE(page.backItems(5).first().index, 0);
    QCOMPARE(page.forwardItems(5).count(), 1);
    QCOMPARE(page.forwardItems(5).first().index, 2);
    page.goToHistoryEntry(page.historyItems().first());
    QCOMPARE(page.lastGoTo, 0);
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

    // The lifted entry point is a separate capability — a backend
    // with no script gate may alias it to runJavaScript, but chrome
    // calls it by name.
    page.runJavaScriptLifted(QStringLiteral("2+2"),
        [&seen](const QVariant &value) { seen = value; });
    QCOMPARE(page.liftedCalls, 1);
    QCOMPARE(page.lastScript, QStringLiteral("2+2"));
    QCOMPARE(seen.toString(), QStringLiteral("fake-lifted"));

    page.m_markup = QStringLiteral("<html>fake</html>");
    QString markup;
    page.toHtml([&markup](const QString &html) { markup = html; });
    QCOMPARE(markup, QStringLiteral("<html>fake</html>"));

    page.setPageAttribute(QStringLiteral("JavascriptEnabled"), false);
    QCOMPARE(page.attributes.value(QStringLiteral("JavascriptEnabled")), false);

    // Per-page script collection + download + view: the neutral
    // surface reaches the fake 1:1.
    page.insertScript(Engine::Script{
        QStringLiteral("arora:test"), QStringLiteral("1"),
        Engine::InjectionPoint::DocumentReady, 0, false });
    QCOMPARE(page.scripts().count(), 1);
    QCOMPARE(page.scripts().first().name, QStringLiteral("arora:test"));
    page.removeScript(QStringLiteral("arora:test"));
    QVERIFY(page.scripts().isEmpty());
    page.download(QUrl(QStringLiteral("https://example.org/f.zip")));
    QCOMPARE(page.lastDownload,
             QUrl(QStringLiteral("https://example.org/f.zip")));
    QCOMPARE(page.view(), static_cast<QWidget*>(nullptr));
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

void tst_EngineAdapter::fakeDownloadSurface()
{
    // The download handoff: a profile emits the request, chrome
    // drives accept/cancel/pause and watches the translated signals.
    FakeProfile profile;
    auto *request = new FakeDownloadRequest(&profile);
    request->m_url = QUrl(QStringLiteral("https://example.org/f.zip"));
    request->m_name = QStringLiteral("f.zip");
    request->m_mime = QStringLiteral("application/zip");

    Engine::DownloadRequest *seen = nullptr;
    connect(&profile, &Engine::Profile::downloadRequested,
            this, [&seen](Engine::DownloadRequest *request) {
        seen = request;
    });
    emit profile.downloadRequested(request);
    QCOMPARE(seen, static_cast<Engine::DownloadRequest*>(request));
    QCOMPARE(seen->url(), request->m_url);
    QCOMPARE(seen->suggestedFileName(), QStringLiteral("f.zip"));
    QCOMPARE(seen->mimeType(), QStringLiteral("application/zip"));
    QCOMPARE(seen->state(), Engine::DownloadRequest::State::Requested);
    QVERIFY(!seen->isFinished());

    QSignalSpy stateSpy(seen, &Engine::DownloadRequest::stateChanged);
    QSignalSpy bytesSpy(seen, &Engine::DownloadRequest::receivedBytesChanged);
    QSignalSpy totalSpy(seen, &Engine::DownloadRequest::totalBytesChanged);
    seen->setDownloadDirectory(QStringLiteral("/tmp/dl"));
    seen->setDownloadFileName(QStringLiteral("g.zip"));
    QCOMPARE(request->m_dir, QStringLiteral("/tmp/dl"));
    QCOMPARE(request->m_fileName, QStringLiteral("g.zip"));
    seen->accept(QStringLiteral("/tmp/dl/g.zip"));
    QCOMPARE(request->acceptedPath, QStringLiteral("/tmp/dl/g.zip"));
    request->bump(10, 100);
    request->bump(20, 100);
    QCOMPARE(bytesSpy.count(), 2);
    QCOMPARE(totalSpy.count(), 1);
    QCOMPARE(seen->receivedBytes(), qint64(20));
    QCOMPARE(seen->totalBytes(), qint64(100));
    seen->pause();
    seen->resume();
    QCOMPARE(request->pauses, 1);
    QCOMPARE(request->resumes, 1);
    request->setState(FakeDownloadRequest::State::Completed);
    QCOMPARE(stateSpy.count(), 2); // InProgress on accept, Completed here
    QVERIFY(seen->isFinished());
    request->setState(FakeDownloadRequest::State::Cancelled);
    QCOMPARE(stateSpy.count(), 3);
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

    // Standard actions resolve to the page's own QActions; the
    // loading/audible/OTR probes are passthroughs.
    QCOMPARE(enginePage->action(Engine::StandardAction::Back),
             view.page()->action(QWebEnginePage::Back));
    QCOMPARE(enginePage->action(Engine::StandardAction::InspectElement),
             view.page()->action(QWebEnginePage::InspectElement));
    QCOMPARE(enginePage->isLoading(), view.page()->isLoading());
    QCOMPARE(enginePage->recentlyAudible(),
             view.page()->recentlyAudible());
    QCOMPARE(enginePage->isOffTheRecord(),
             view.page()->profile()->isOffTheRecord());

    // The history surface mirrors the wrapped history 1:1.
    QCOMPARE(enginePage->historyCount(), view.page()->history()->count());
    QCOMPARE(enginePage->currentHistoryIndex(),
             view.page()->history()->currentItemIndex());
    const QList<Engine::HistoryEntry> items = enginePage->historyItems();
    QCOMPARE(items.count(), view.page()->history()->items().count());
    for (int i = 0; i < items.count(); ++i) {
        QCOMPARE(items.at(i).index, i);
        QCOMPARE(items.at(i).url,
                 view.page()->history()->items().at(i).url());
    }
    const int back = enginePage->currentHistoryIndex();
    QCOMPARE(enginePage->backItems(10).count(), back);
    QCOMPARE(enginePage->forwardItems(10).count(),
             enginePage->historyCount() - back - 1);

    // toHtml forwards to the wrapped page — the data: document's
    // serialized DOM arrives asynchronously from the render process.
    QString markup;
    enginePage->toHtml([&markup](const QString &html) { markup = html; });
    QTRY_VERIFY_WITH_TIMEOUT(markup.contains(QLatin1String("ENG04")),
                             15000);

    // The escape-hatch downcast resolves on this backend's adapter.
    QCOMPARE(WebEnginePageAdapter::of(enginePage)->webEnginePage(),
             view.page());
    QCOMPARE(WebEnginePageAdapter::of(nullptr),
             static_cast<WebEnginePageAdapter*>(nullptr));

    // Canonical adapters: every holder of the same engine page
    // resolves to the same Engine::Page (identity-comparable), and
    // view() is the engine's forPage reverse lookup.
    QCOMPARE(WebEnginePageAdapter::forPage(view.page()),
             static_cast<WebEnginePageAdapter*>(enginePage));
    QCOMPARE(enginePage->view(), static_cast<QWidget*>(&view));

    // The per-page script collection round-trips through the engine —
    // the page already arms its own bootstrap scripts, so assert the
    // named entry lands and leaves rather than a bare count.
    const int before = enginePage->scripts().count();
    enginePage->insertScript(Engine::Script{
        QStringLiteral("arora:eng04-test"), QStringLiteral("void 0"),
        Engine::InjectionPoint::DocumentReady, 0, false });
    QCOMPARE(enginePage->scripts().count(), before + 1);
    QCOMPARE(view.page()->scripts().toList().count(), before + 1);
    bool found = false;
    for (const Engine::Script &script : enginePage->scripts())
        found |= script.name == QStringLiteral("arora:eng04-test");
    QVERIFY(found);
    enginePage->removeScript(QStringLiteral("arora:eng04-test"));
    QCOMPARE(enginePage->scripts().count(), before);
}

void tst_EngineAdapter::webEngineLiftedScript()
{
    // runJavaScriptLifted is the chrome-script path for JSCTL-blocked
    // pages: with scripting on it is the plain evaluation; with
    // scripting off the backend lifts the gate, runs, and restores.
    WebView view;
    Engine::Page *enginePage = view.enginePage();
    QSignalSpy loadSpy(enginePage, &Engine::Page::loadFinished);
    enginePage->load(
        QUrl(QStringLiteral("data:text/html,<title>lift</title>")));
    QTRY_VERIFY_WITH_TIMEOUT(loadSpy.count() >= 1, 15000);

    QVariant seen;
    enginePage->runJavaScriptLifted(QStringLiteral("1+1"),
        [&seen](const QVariant &value) { seen = value; });
    QTRY_VERIFY_WITH_TIMEOUT(seen.isValid(), 15000);
    QCOMPARE(seen.toInt(), 2);

    // Scripts off: the lifted path still lands (queued behind the
    // attribute IPC, hence the generous timeout) and the gate is
    // restored afterwards.
    view.page()->settings()->setAttribute(
        QWebEngineSettings::JavascriptEnabled, false);
    seen = QVariant();
    enginePage->runJavaScriptLifted(QStringLiteral("6*7"),
        [&seen](const QVariant &value) { seen = value; });
    QTRY_VERIFY_WITH_TIMEOUT(seen.isValid(), 15000);
    QCOMPARE(seen.toInt(), 42);
    QTRY_VERIFY_WITH_TIMEOUT(
        !view.page()->settings()->testAttribute(
            QWebEngineSettings::JavascriptEnabled), 15000);

    // A null page handle refuses cleanly through the static hatch.
    bool called = false;
    WebEnginePageAdapter::runJavaScriptLiftedOn(nullptr,
        QStringLiteral("void 0"),
        [&called](const QVariant &) { called = true; });
    QVERIFY(called);
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
