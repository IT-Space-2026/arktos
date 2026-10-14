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
#include <qlabel.h>

#include <engineinterface.h>
#include <enginetab.h>
#include <statusbarwidgets.h>
#include <webenginebackend.h>
#include <webview.h>

#include "qtest_arora.h"
#include "fakeengine.h"

// The reference fake backend lives in autotests/fakeengine.h — shared
// with tst_engineswitch and tst_tabwidget's swap legs (ENG06: one
// reference backend for the whole engine surface).

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
    void memIndicatorContract();
    void webEngineBackendShape();
    void webEnginePageForwarding();
    void webEngineLiftedScript();
    void webEngineProfileSurface();
};

void tst_EngineAdapter::fakeBackendShape()
{
    FakeEngineBackend backend;
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
    FakeEnginePage page;
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
    FakeEnginePage page;

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
    FakeEnginePage page;
    QCOMPARE(page.lifecycleState(), Engine::Page::LifecycleState::Active);
    page.setLifecycleState(Engine::Page::LifecycleState::Discarded);
    QCOMPARE(page.lifecycleState(), Engine::Page::LifecycleState::Discarded);

    // Passthrough shape: -1 means "engine has no per-page process".
    QCOMPARE(page.renderProcessId(), qint64(-1));
    page.setRenderProcessId(4242);
    QCOMPARE(page.renderProcessId(), qint64(4242));

    FakeEnginePage child;
    page.windowResult = &child;
    QCOMPARE(page.createWindow(Engine::WebWindowType::BrowserTab),
             static_cast<Engine::Page*>(&child));
    QCOMPARE(page.lastWindowType, Engine::WebWindowType::BrowserTab);
}

void tst_EngineAdapter::fakeProfileSurface()
{
    FakeEngineProfile profile;
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

    // The NavigationPolicy handoff: chrome installs an
    // Engine::RequestPolicy through the profile and the interface
    // hands the same pointer back (tst_navigationpolicy exercises
    // the policy object itself engine-neutrally).
    QCOMPARE(profile.requestPolicy(), nullptr);
    Engine::RequestPolicy *policy =
        reinterpret_cast<Engine::RequestPolicy*>(quintptr(0xdead));
    profile.setRequestPolicy(policy);
    QCOMPARE(profile.requestPolicy(), policy);

    // And the cookie gate installs the same way — the lambda is
    // stashed, not eaten.
    QVERIFY(!profile.m_filter);
    profile.setCookieFilter(
        [](const Engine::CookieAttempt &) { return false; });
    QVERIFY(profile.m_filter);
}

void tst_EngineAdapter::fakeDownloadSurface()
{
    // The download handoff: a profile emits the request, chrome
    // drives accept/cancel/pause and watches the translated signals.
    FakeEngineProfile profile;
    auto *request = new FakeEngineDownloadRequest(&profile);
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
    request->setState(FakeEngineDownloadRequest::State::Completed);
    QCOMPARE(stateSpy.count(), 2); // InProgress on accept, Completed here
    QVERIFY(seen->isFinished());
    request->setState(FakeEngineDownloadRequest::State::Cancelled);
    QCOMPARE(stateSpy.count(), 3);
}

// ENG06(d) — the SBAR01 memory widget against the interface's
// process-identity contract, on both backend shapes:
//  * a page reporting -1 ("engine has no per-page process") must
//    yield the '—' placeholder, not a bogus reading;
//  * ENG26: an in-process page bound via EngineTab reports the
//    shared host figure flagged memoryIsShared() — the label shows
//    it with an honest "(shared)" tag, never a fake per-tab number;
//  * a real WebEngine page reports a live renderer pid and the
//    label shows an actual "MEM <size>" reading.
void tst_EngineAdapter::memIndicatorContract()
{
    // Static helpers: dead/bogus pids read as -1, self reads a real
    // VmRSS; the formatter's unit ladder is pinned.
    QCOMPARE(MemIndicator::residentMemoryKb(-1), qint64(-1));
    QCOMPARE(MemIndicator::residentMemoryKb(0), qint64(-1));
    QCOMPARE(MemIndicator::residentMemoryKb(999999999), qint64(-1));
#if defined(Q_OS_LINUX)
    QVERIFY(MemIndicator::residentMemoryKb(
                QCoreApplication::applicationPid()) > 0);
#endif
    QCOMPARE(MemIndicator::formatRss(87 * 1024 + 400),
             QStringLiteral("87.4 MB"));
    QCOMPARE(MemIndicator::formatRss(238 * 1024),
             QStringLiteral("238 MB"));
    QCOMPARE(MemIndicator::formatRss(1536 * 1024),
             QStringLiteral("1.5 GB"));

    // Fake backend: renderProcessId() == -1 is the documented
    // "no per-page process" answer — MemIndicator::refresh() turns it
    // into the placeholder.
    FakeEnginePage fakePage;
    QCOMPARE(fakePage.renderProcessId(), qint64(-1));
    QCOMPARE(MemIndicator::residentMemoryKb(fakePage.renderProcessId()),
             qint64(-1));
    fakePage.setRenderProcessId(4242);
    QCOMPARE(fakePage.renderProcessId(), qint64(4242));
    // A staged pid that no longer exists still reads '—', never
    // a fabricated figure.
    QCOMPARE(MemIndicator::residentMemoryKb(4242), qint64(-1));

    MemIndicator indicator;
    QLabel *label = indicator.findChild<QLabel *>(
        QStringLiteral("memLabel"));
    QVERIFY(label);
    QCOMPARE(label->text(), QStringLiteral("MEM —"));
    QCOMPARE(indicator.webView(), static_cast<WebView*>(nullptr));

    // ENG26: a foreign-engine tab hosts its page without a WebView,
    // so the indicator binds the EngineTab and re-resolves page() on
    // every poll.  An in-process page reports the shared host figure
    // flagged memoryIsShared() — the label carries the honest
    // "(shared)" tag instead of a fake per-tab reading.
    FakeEngineBackend sharedBackend;
    Engine::Profile *sharedProfile = sharedBackend.createProfile(
        Engine::ProfileOptions(), &sharedBackend);
    EngineTab foreignTab(&sharedBackend, sharedProfile);
    auto *inProcessPage =
        static_cast<FakeEnginePage *>(foreignTab.page());
    QVERIFY(inProcessPage);
    inProcessPage->m_memoryShared = true;
    inProcessPage->m_residentKb = 238 * 1024;

    MemIndicator sharedIndicator;
    sharedIndicator.setEngineTab(&foreignTab);
    QCOMPARE(sharedIndicator.engineTab(), &foreignTab);
    QCOMPARE(sharedIndicator.webView(), static_cast<WebView*>(nullptr));
    QLabel *sharedLabel = sharedIndicator.findChild<QLabel *>(
        QStringLiteral("memLabel"));
    QVERIFY(sharedLabel);
    QCOMPARE(sharedLabel->text(),
             QStringLiteral("MEM 238 MB (shared)"));
    QVERIFY(sharedLabel->toolTip().contains(
        QStringLiteral("in-process")));

    // ENG28 path: an engine restart swaps the page object in place —
    // the bound indicator picks the fresh page up on pageRecreated,
    // not a dangling pointer.  The replacement fake reports no
    // memory surface, so the label falls back to '—'.  (A new page
    // object is proven via the backend's bookkeeping — the allocator
    // may hand the just-freed address back.)
    QVERIFY(foreignTab.restartEngine());
    QCOMPARE(sharedBackend.pages.count(), 2);
    QVERIFY(sharedBackend.pages.at(0).isNull());
    QCOMPARE(sharedLabel->text(), QStringLiteral("MEM —"));

    // Real backend: a loaded WebEngine page reports a live renderer
    // pid through enginePage(), and the widget shows the reading.
    WebView view;
    Engine::Page *enginePage = view.enginePage();
    QVERIFY(enginePage);
    QSignalSpy loadSpy(enginePage, &Engine::Page::loadFinished);
    enginePage->load(
        QUrl(QStringLiteral("data:text/html,<title>mem</title>")));
    QTRY_VERIFY_WITH_TIMEOUT(loadSpy.count() >= 1, 15000);
#if defined(Q_OS_LINUX)
    QVERIFY(enginePage->renderProcessId() > 0);
    indicator.setWebView(&view);
    QCOMPARE(indicator.webView(), &view);
    QVERIFY(label->text() != QStringLiteral("MEM —"));
    QVERIFY(label->text().startsWith(QLatin1String("MEM ")));

    // Switching engines rebinds cleanly both ways: WebView off the
    // shared tab drops the "(shared)" tag, and binding the tab again
    // restores it.
    sharedIndicator.setWebView(&view);
    QCOMPARE(sharedIndicator.webView(), &view);
    QCOMPARE(sharedIndicator.engineTab(),
             static_cast<EngineTab *>(nullptr));
    QVERIFY(sharedLabel->text().startsWith(QLatin1String("MEM ")));
    QVERIFY(!sharedLabel->text().contains(
        QStringLiteral("(shared)")));
    sharedIndicator.setEngineTab(&foreignTab);
    QCOMPARE(sharedIndicator.engineTab(), &foreignTab);
    // The restarted page reports no memory surface at all.
    QCOMPARE(sharedLabel->text(), QStringLiteral("MEM —"));
#else
    Q_UNUSED(indicator);
    QSKIP("/proc-based widget is Linux-only");
#endif
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
