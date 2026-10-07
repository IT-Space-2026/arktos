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

#include "adblockmanager.h"
#include "adblocknetwork.h"
#include "adblockresourcehandler.h"
#include "adblockrule.h"
#include "adblockschemeaccesshandler.h"
#include "adblocksubscription.h"
#include "acceptlanguagedialog.h"
#include "autofillmanager.h"
#include "bookmarknode.h"
#include "bookmarksmanager.h"
#include "bookmarksmodel.h"
#include "browserapplication.h"
#include "browsermainwindow.h"
#include "browserpaths.h"
#include "browserprofile.h"
#include "clearprivatedata.h"
#include "cookiejar.h"
#include "downloadmanager.h"
#include "extensionmanager.h"
#include "history.h"
#include "historymanager.h"
#include "historyparser.h"
#include "locationbar.h"
#include "networkaccessmanager.h"
#include "opensearchengine.h"
#include "opensearchmanager.h"
#include "opensearchreader.h"
#include "opensearchwriter.h"
#include "plaintexteditsearch.h"
#include "schemeaccesshandler.h"
#include "securestore.h"
#include "settings.h"
#include "sourcehighlighter.h"
#include "sourceviewer.h"
#include "tabwidget.h"
#include "toolbarsearch.h"
#include "webpage.h"
#include "webview.h"
#include "webviewsearch.h"
#include "xbelreader.h"
#include "xbelwriter.h"

#include <QtCore/QBuffer>
#include <QtCore/QCommandLineParser>
#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QElapsedTimer>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtCore/QXmlStreamReader>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtGui/QAbstractTextDocumentLayout>
#include <QtGui/QIcon>
#include <QtGui/QMouseEvent>
#include <QtGui/QPixmap>
#include <QtGui/QTextDocument>
#include <QtGui/QTextLayout>
#include <QtNetwork/QNetworkCookie>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtWebEngineCore/QWebEngineDownloadRequest>
#include <QtWebEngineCore/QWebEngineFindTextResult>
#include <QtWebEngineCore/QWebEngineLoadingInfo>
#include <QtWebEngineCore/QWebEngineProfile>
#include <QtWebEngineCore/QWebEngineScriptCollection>
#include <QtWebEngineCore/QWebEngineSettings>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QToolButton>

#include <cstdio>
#include <cstdlib>
#include <memory>

#if defined(ARORA_ADBLOCK_RUST)
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>

// Normalizes a matcher decision for --adblock-rust-smoke:
// 0 allow, 1 block, 2 stub redirect (bundled resource name or data:
// URL), 3 rewritten request URL ($removeparam — the native matcher
// emits Allow + removeParams, the Rust engine emits Redirect to the
// stripped URL).
static int adblockDecisionKind(const AdBlockDecision &decision)
{
    if (decision.action == AdBlockDecision::Redirect) {
        if (!decision.redirectUrl.isEmpty()
            && !decision.redirectUrl.startsWith(QLatin1String("data:")))
            return 3;
        return 2;
    }
    if (decision.action == AdBlockDecision::Allow
        && !decision.removeParams.isEmpty())
        return 3;
    return int(decision.action);
}
#endif

int main(int argc, char **argv)
{
    // Zero-cost wall clock for --perf-smoke's cold-start checkpoints.
    QElapsedTimer perfTimer;
    perfTimer.start();

    Q_INIT_RESOURCE(htmls);
    Q_INIT_RESOURCE(data);

    // Custom URL schemes must be registered before the application
    // exists: arora-file:// directory listings (MIG04), abp:subscribe
    // adblock links (MIG09), arora-resource:// bundled $redirect= stubs
    // (ADB01).  Apart from this constraint Qt6 WebEngineWidgets needs
    // no explicit initialize() call — QtWebEngineQuick::initialize()
    // is for the Quick module only; the engine spins up lazily with
    // the first page.
    SchemeAccessHandler::registerUrlSchemes();
    AdBlockSchemeAccessHandler::registerUrlScheme();
    AdBlockResourceHandler::registerUrlScheme();

    // The --*-smoke development runs keep their writes out of the
    // user's real data and settings locations.  argv is scanned before
    // the application exists because the BrowserApplication
    // constructor already brings up the browsing profile and its
    // managers.
    bool smokeRun = false;
    for (int i = 1; i < argc; ++i) {
        const QByteArray arg(argv[i]);
        if (arg.startsWith("--") && arg.endsWith("-smoke"))
            smokeRun = true;
    }
    if (smokeRun)
        QStandardPaths::setTestModeEnabled(true);

    BrowserApplication application(argc, argv);
    const qint64 appCtorMs = perfTimer.elapsed();

    // A non-standalone run that could not take the single-instance
    // socket already forwarded its url to the running instance and is
    // done.  Standalone runs (any --option) never join the handshake.
    if (!application.isStandalone() && !application.isRunning())
        return 0;

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QCoreApplication::translate("main",
            "Arora — a lightweight cross-platform web browser."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(
        QLatin1String("url"),
        QCoreApplication::translate("main", "Url to open on startup."),
        QStringLiteral("[url...]"));
    // Internal development/verification flags.
    const char *const internalOptions[] = {
        "quit-after-load",
        "nam-smoke", "history-smoke", "download-smoke", "cookie-smoke",
        "bookmarks-smoke", "search-smoke", "adblock-smoke",
        "adblock-list-smoke", "adblock-rust-smoke", "autofill-smoke",
        "settings-smoke", "find-smoke", "source-smoke", "browser-smoke",
        "app-smoke", "extension-smoke", "ua-smoke", "perf-smoke",
        "session-smoke", "restore-smoke",
    };
    for (const char *option : internalOptions)
        parser.addOption(QCommandLineOption(QLatin1String(option)));
    parser.process(application);

    if (!application.isStandalone()) {
        // Normal launch: postLaunch() (queued by the constructor)
        // applies the startup behavior — homepage, last-session
        // restore or the url operand — to this first window.
        application.newMainWindow();
        return application.exec();
    }

    // Standalone development harness: a stub window hosting a WebView
    // on the browsing profile drives --quit-after-load and the smokes.
    const QStringList args = application.arguments();

    QWebEngineProfile *profile = BrowserApplication::webEngineProfile();
    CookieJar *cookieJar = CookieJar::instance(profile);
    NetworkAccessManager *networkAccessManager =
        BrowserApplication::networkAccessManager();
    DownloadManager *downloadManager = BrowserApplication::downloadManager();

    QMainWindow window;
    window.setWindowTitle(QStringLiteral("Arora"));

    WebView *view = new WebView(profile, &window);
    window.setCentralWidget(view);

    QUrl firstUrl(parser.positionalArguments()
        .value(0, QStringLiteral("about:blank")));
    // SEC09: argv urls are untrusted input — a javascript: operand
    // must not reach loadUrl()'s script execution path.
    if (!WebView::isUrlAllowedOnUntrustedInput(firstUrl)) {
        qWarning() << "Ignoring untrusted argv url:" << firstUrl;
        firstUrl = QUrl(QStringLiteral("about:blank"));
    }
    view->loadUrl(firstUrl);

    // Headless verification for MIG15: exercise the real application
    // path — BrowserApplication brings up the profile and services,
    // opens a BrowserMainWindow and a tab that loads a fixture page,
    // the title propagating to the window.  Exits 0 on PASS.
    if (args.contains(QLatin1String("--app-smoke"))) {
        // Suppress postLaunch()'s startup behavior (goHome / session
        // restore) so the fixture load is the only navigation.
        QSettings().setValue(QLatin1String("MainWindow/startupBehavior"), 1);
        BrowserMainWindow *browserWindow = application.newMainWindow();
        WebView *tab = browserWindow->currentTab();

        const QString fixturePath = QDir::temp().filePath(
            QLatin1String("arora-app-smoke.html"));
        {
            QFile fixture(fixturePath);
            if (!fixture.open(QIODevice::WriteOnly)) {
                qInfo() << "app-smoke: FAIL (cannot write fixture)";
                return 1;
            }
            fixture.write("<html><head><title>app-smoke-page</title>"
                          "</head><body>app</body></html>");
        }
        const QUrl fixtureUrl = QUrl::fromLocalFile(fixturePath);

        QObject::connect(tab, &QWebEngineView::loadFinished, &application,
                         [&application, tab, fixtureUrl, fixturePath,
                          browserWindow](bool ok) {
            if (!ok || tab->url() != fixtureUrl)
                return;
            const bool pass = browserWindow->windowTitle()
                .contains(QLatin1String("app-smoke-page"));
            qInfo() << "app-smoke:" << (pass ? "PASS" : "FAIL")
                    << tab->url() << browserWindow->windowTitle();
            QFile::remove(fixturePath);
            application.exit(pass ? 0 : 1);
        });
        QTimer::singleShot(15000, &application, [&application]() {
            qInfo() << "app-smoke: FAIL (timeout)";
            application.exit(1);
        });
        browserWindow->tabWidget()->loadUrl(fixtureUrl,
                                            TabWidget::CurrentTab);
    }

    // Headless verification hook: exit once the first page load
    // succeeds so CI can prove WebEngine ran (autotests/smoke style).
    // Failed navigations are skipped so the file:// -> arora-file://
    // directory-listing redirect still counts when it completes.
    if (args.contains(QLatin1String("--quit-after-load"))) {
        QObject::connect(view, &QWebEngineView::loadFinished,
                         &application, [view, &application](bool ok) {
            if (!ok)
                return;
            qInfo() << "loadFinished:" << view->url() << view->title();
            application.exit(0);
        });
        QTimer::singleShot(15000, &application,
                           [&application]() { application.exit(1); });
    }

    window.show();

    // Headless verification for MIG04: the app-side NAM performs a
    // local file:// GET through its proxy factory, disk cache, cookie
    // jar and Accept-Language injection.  Exits 0 on success.
    if (args.contains(QLatin1String("--nam-smoke"))) {
        QNetworkRequest request(QUrl::fromLocalFile(QStringLiteral("/etc/hostname")));
        QNetworkReply *reply = networkAccessManager->get(request);
        QObject::connect(reply, &QNetworkReply::finished, &application,
                         [&application, reply]() {
            qInfo() << "nam-smoke:" << reply->error() << reply->url();
            application.exit(reply->error() == QNetworkReply::NoError ? 0 : 1);
        });
    }

    // Headless verification for MIG06: a successful main-frame load
    // must land in the app-side HistoryManager — WebEngine doesn't push
    // visited urls into the app like QWebHistoryInterface did.  Exits 0
    // when the loaded url is found in history.
    if (args.contains(QLatin1String("--history-smoke"))) {
        QObject::connect(view, &QWebEngineView::loadFinished, &application,
                         [view, &application](bool ok) {
            if (!ok)
                return;
            const QString urlString = view->url().toString();
            const bool found =
                HistoryManager::instance()->historyContains(urlString);
            qInfo() << "history-smoke:" << (found ? "PASS" : "FAIL") << urlString
                    << "entries:" << HistoryManager::instance()->history().count();
            application.exit(found ? 0 : 1);
        });
        QTimer::singleShot(15000, &application,
                           [&application]() { application.exit(1); });
    }

    // Headless verification for MIG05: issue a real WebEngine download
    // of the given URL.  The DownloadManager picks a file name inside a
    // scratch download dir; exit 0 once the file lands on disk.
    const int downloadSmokeIndex = args.indexOf(QLatin1String("--download-smoke"));
    if (downloadSmokeIndex != -1 && args.count() > downloadSmokeIndex + 1) {
        const QUrl downloadUrl(args.at(downloadSmokeIndex + 1));
        const QString smokeDir =
            QDir::temp().filePath(QLatin1String("arora-download-smoke"));
        QDir().mkpath(smokeDir);
        downloadManager->setDownloadDirectory(smokeDir);
        // A save-as prompt can't be answered under offscreen QPA.
        QSettings().setValue(
            QLatin1String("downloadmanager/alwaysPromptForFileName"), false);
        QObject::connect(profile, &QWebEngineProfile::downloadRequested,
                         &application,
                         [&application](QWebEngineDownloadRequest *request) {
            QObject::connect(request, &QWebEngineDownloadRequest::stateChanged,
                             &application,
                             [&application, request](QWebEngineDownloadRequest::DownloadState state) {
                if (state == QWebEngineDownloadRequest::DownloadCompleted) {
                    const QString path = request->downloadDirectory()
                        + QLatin1Char('/') + request->downloadFileName();
                    const bool ok = QFile::exists(path) && QFileInfo(path).size() > 0;
                    qInfo() << "download-smoke:" << (ok ? "PASS" : "FAIL")
                            << path << request->receivedBytes() << "bytes";
                    application.exit(ok ? 0 : 1);
                } else if (state == QWebEngineDownloadRequest::DownloadInterrupted
                           || state == QWebEngineDownloadRequest::DownloadCancelled) {
                    qInfo() << "download-smoke: FAIL"
                            << request->interruptReasonString();
                    application.exit(1);
                }
            });
        });
        QTimer::singleShot(30000, &application,
                           [&application]() { application.exit(1); });
        view->webPage()->download(downloadUrl);
    }

    // Headless verification for MIG03: push a cookie through the jar's
    // app-side API, verify the store mirror picks it up and that a
    // blocked-domain cookie is rejected. Exits 0 on PASS.
    if (args.contains(QLatin1String("--cookie-smoke"))) {
        QTimer::singleShot(0, &application, [cookieJar]() {
            QNetworkCookie allowed("arora_smoke", "1");
            allowed.setDomain(QLatin1String("example.com"));
            cookieJar->setCookiesFromUrl(QList<QNetworkCookie>() << allowed,
                                         QUrl(QLatin1String("http://example.com/")));

            cookieJar->setBlockedCookies(QStringList() << QLatin1String("blocked.example"));
            QNetworkCookie blocked("arora_blocked", "1");
            blocked.setDomain(QLatin1String("blocked.example"));
            cookieJar->setCookiesFromUrl(QList<QNetworkCookie>() << blocked,
                                         QUrl(QLatin1String("http://blocked.example/")));
        });
        // The cookie store mirror updates asynchronously via
        // cookieAdded — poll for the allowed cookie instead of
        // checking once at a fixed delay (marginal on slow builds).
        QTimer *cookiePoll = new QTimer(&application);
        auto cookiePollTicks = std::make_shared<int>(0);
        QObject::connect(cookiePoll, &QTimer::timeout, &application,
                         [&application, cookieJar, cookiePoll,
                          cookiePollTicks]() {
            const QList<QNetworkCookie> allowed =
                cookieJar->cookiesForUrl(QUrl(QLatin1String("http://example.com/")));
            const QList<QNetworkCookie> blocked =
                cookieJar->cookiesForUrl(QUrl(QLatin1String("http://blocked.example/")));
            bool found = false;
            for (const QNetworkCookie &cookie : allowed)
                found |= (cookie.name() == "arora_smoke");
            const bool pass = blocked.isEmpty() && found;
            if (!pass && ++*cookiePollTicks <= 20)
                return; // retry for ~10s
            qInfo() << "cookie-smoke:" << (pass ? "PASS" : "FAIL")
                    << "(allowed:" << allowed.count() << "blocked:" << blocked.count() << ")";
            cookiePoll->stop();
            // leave no test residue in the saved exception list
            cookieJar->setBlockedCookies(QStringList());
            application.exit(pass ? 0 : 1);
        });
        cookiePoll->start(500);
    }

    // Headless verification for MIG07: exercise the app-wide bookmarks
    // store — load (falls back to the bundled default XBEL), add /
    // rename / remove a bookmark through the undo-stack API while the
    // model watches, plus an XBEL round-trip and &nbsp; entity
    // expansion (Qt6 dropped QXmlStreamEntityResolver).  Exits 0 on PASS.
    if (args.contains(QLatin1String("--bookmarks-smoke"))) {
        QTimer::singleShot(0, &application, [&application]() {
            BookmarksManager *manager = BookmarksManager::instance();
            BookmarksModel *model = manager->bookmarksModel();
            BookmarkNode *menu = manager->menu();
            const int before = menu->children().count();

            BookmarkNode *node = new BookmarkNode(BookmarkNode::Bookmark);
            node->title = QStringLiteral("smoke");
            node->url = QStringLiteral("http://example.com/");
            manager->addBookmark(menu, node);
            const bool added = menu->children().count() == before + 1
                && model->data(model->index(node), Qt::DisplayRole)
                       .toString() == QLatin1String("smoke");
            manager->setTitle(node, QStringLiteral("smoke2"));
            const bool renamed = node->title == QLatin1String("smoke2");
            manager->removeBookmark(node);
            const bool removed = menu->children().count() == before;

            // XBEL round-trip through a temp file
            const QString tmpFile = QDir::temp().filePath(
                QLatin1String("arora-bookmarks-smoke.xbel"));
            XbelWriter writer;
            const bool wrote = writer.write(tmpFile, manager->bookmarks());
            XbelReader reader;
            BookmarkNode *copy = reader.read(tmpFile);
            const bool roundtrip = wrote
                && reader.error() == QXmlStreamReader::NoError
                && copy->children().count()
                       == manager->bookmarks()->children().count();
            delete copy;
            QFile::remove(tmpFile);

            // &nbsp; expansion (the pre-Qt6 entity resolver's job);
            // it expands to a plain space, matching the Qt4 resolver and
            // the autotests/xbel/all.xbel fixture expectation.
            QByteArray xbel =
                "<xbel><folder folded=\"no\"><title>a&nbsp;b</title>"
                "</folder></xbel>";
            QBuffer buffer(&xbel);
            buffer.open(QIODevice::ReadOnly);
            XbelReader entityReader;
            BookmarkNode *entityRoot = entityReader.read(&buffer);
            const bool entity = entityReader.error() == QXmlStreamReader::NoError
                && entityRoot->children().count() == 1
                && entityRoot->children().first()->title
                       == QLatin1String("a b");
            delete entityRoot;

            const bool ok = added && renamed && removed && roundtrip && entity;
            qInfo() << "bookmarks-smoke:" << (ok ? "PASS" : "FAIL")
                    << "(added:" << added << "renamed:" << renamed
                    << "removed:" << removed << "xbel:" << roundtrip
                    << "entity:" << entity << ")";
            application.exit(ok ? 0 : 1);
        });
    }

    // Headless verification for MIG08: the bundled OpenSearch
    // descriptions load from the resource, template substitution
    // produces a valid search url, the XML round-trips through
    // writer+reader, keyword search resolves, and a suggestion query
    // against a local file:// reply is parsed via QJsonDocument (the
    // QtScript eval path is gone).  Exits 0 on PASS.
    if (args.contains(QLatin1String("--search-smoke"))) {
        OpenSearchManager *manager = ToolbarSearch::openSearchManager();
        OpenSearchEngine *google = manager->engine(QLatin1String("Google"));
        bool ok = manager->enginesCount() >= 6 && google && google->isValid()
                && manager->currentEngine();
        if (google) {
            const QUrl url = google->searchUrl(QLatin1String("hello world"));
            ok = ok && url.isValid()
                 && QString::fromUtf8(url.toEncoded())
                        .contains(QLatin1String("hello%20world"));
        }

        // Writer + reader round-trip of a bundled engine.
        QByteArray xml;
        QBuffer writeBuffer(&xml);
        writeBuffer.open(QIODevice::WriteOnly);
        OpenSearchWriter writer;
        bool wrote = google && writer.write(&writeBuffer, google);
        writeBuffer.close();
        QBuffer readBuffer(&xml);
        OpenSearchReader reader;
        OpenSearchEngine *copy = reader.read(&readBuffer);
        ok = ok && wrote && reader.error() == QXmlStreamReader::NoError
             && copy->isValid()
             && copy->name() == google->name()
             && copy->searchUrlTemplate() == google->searchUrlTemplate();
        delete copy;

        // Location-bar keyword search ("g terms" -> engine search url).
        manager->setEngineForKeyword(QLatin1String("g"), google);
        ok = ok && manager->convertKeywordSearchToUrl(
                QLatin1String("g arora")).isValid();

        // The widget side constructs offscreen.
        LocationBar locationBar(&window);
        locationBar.setWebView(view);
        ToolbarSearch toolbarSearch(&window);
        toolbarSearch.setWebView(view);
        ok = ok && toolbarSearch.openSearchManager() == manager;

        if (!ok) {
            qInfo() << "search-smoke: FAIL (engines:" << manager->enginesCount() << ")";
            return 1;
        } else {
            // Suggestions: a throwaway engine pointed at a local file
            // reply exercises the JSON suggestion parser end-to-end.
            const QString fixturePath = QDir::temp().filePath(
                QLatin1String("arora-suggest-smoke.json"));
            QFile fixture(fixturePath);
            if (!fixture.open(QIODevice::WriteOnly)) {
                qInfo() << "search-smoke: FAIL (cannot write fixture)";
                return 1;
            }
            fixture.write("[\"arora\",[\"arora browser\",\"arora git\"]]");
            fixture.close();

            OpenSearchEngine *suggest = new OpenSearchEngine(&application);
            suggest->setName(QLatin1String("smoke"));
            suggest->setSuggestionsUrlTemplate(
                QLatin1String("file://") + fixturePath
                + QLatin1String("?q={searchTerms}"));
            suggest->setNetworkAccessManager(networkAccessManager);
            QObject::connect(suggest, &OpenSearchEngine::suggestions,
                             &application,
                             [&application, fixturePath](const QStringList &suggestions) {
                const bool pass = suggestions.count() == 2
                    && suggestions.at(0) == QLatin1String("arora browser");
                qInfo() << "search-smoke:" << (pass ? "PASS" : "FAIL")
                        << "suggestions:" << suggestions;
                QFile::remove(fixturePath);
                application.exit(pass ? 0 : 1);
            });
            suggest->requestSuggestions(QLatin1String("arora"));
            QTimer::singleShot(15000, &application,
                               [&application]() { application.exit(1); });
        }
    }

    // Headless verification for MIG09: the profile url request
    // interceptor (the only request-blocking surface under WebEngine)
    // must fail a request matching a custom rule (info.block() ->
    // net::ERR_ACCESS_DENIED/ERR_BLOCKED_BY_CLIENT); an @@ exception
    // must let the request through to fail on its own (any other net
    // error proves the interceptor did not stop it); and a ## cosmetic
    // filter must be injected as a <style> element into a loaded page.
    // Exits 0 on
    // PASS for all three stages.  App-data writes are isolated by the
    // QStandardPaths test mode enabled earlier in main().
    // Lives at function scope: the loadFinished/loadingChanged lambdas
    // below capture it by reference and fire inside exec(), after the
    // smoke's if-block has already closed.
    int adblockSmokeStage = 0;
    if (args.contains(QLatin1String("--adblock-smoke"))) {
        AdBlockManager *manager = AdBlockManager::instance();
        AdBlockSubscription *custom = manager->customRules();
        custom->addRule(AdBlockRule(QLatin1String("||adblock-smoke.invalid^")));
        custom->addRule(AdBlockRule(QLatin1String(".invalid^")));
        custom->addRule(AdBlockRule(QLatin1String("@@||allowed-smoke.invalid^")));
        custom->addRule(AdBlockRule(QLatin1String("##body")));

        // Snapshot-level check of what the IO-thread matcher sees.
        AdBlockNetwork *network = manager->network();
        const bool matcherOk =
            network->shouldBlock(QUrl(QLatin1String("http://adblock-smoke.invalid/banner.js")))
            && !network->shouldBlock(QUrl(QLatin1String("http://allowed-smoke.invalid/page.js")))
            && !network->shouldBlock(QUrl(QLatin1String("http://example.com/page.js")));
        qInfo() << "adblock-smoke: matcher" << (matcherOk ? "PASS" : "FAIL");
        if (!matcherOk)
            return 1;

        QObject::connect(view->webPage(), &QWebEnginePage::loadingChanged,
                         &application,
                         [view, &application, &adblockSmokeStage](const QWebEngineLoadingInfo &info) {
            if (info.status() != QWebEngineLoadingInfo::LoadFailedStatus)
                return;
            const QString host = info.url().host();
            const bool blockedByInterceptor =
                info.errorString().contains(QLatin1String("ERR_BLOCKED_BY_CLIENT"))
                || info.errorString().contains(QLatin1String("ERR_ACCESS_DENIED"));
            if (adblockSmokeStage == 0 && host == QLatin1String("adblock-smoke.invalid")) {
                qInfo() << "adblock-smoke: blocked navigation"
                        << (blockedByInterceptor ? "PASS" : "FAIL") << info.errorString();
                if (!blockedByInterceptor) {
                    application.exit(1);
                    return;
                }
                adblockSmokeStage = 1;
                view->loadUrl(QUrl(QLatin1String("http://allowed-smoke.invalid/")));
            } else if (adblockSmokeStage == 1 && host == QLatin1String("allowed-smoke.invalid")) {
                const bool pass = !blockedByInterceptor;
                qInfo() << "adblock-smoke: exception navigation"
                        << (pass ? "PASS" : "FAIL") << info.errorString();
                if (!pass) {
                    application.exit(1);
                    return;
                }
                adblockSmokeStage = 2;
                view->loadUrl(QUrl(QLatin1String("about:blank")));
            }
        });
        QObject::connect(view, &QWebEngineView::loadFinished, &application,
                         [view, &application, &adblockSmokeStage](bool ok) {
            if (adblockSmokeStage != 2 || !ok
                || view->url() != QUrl(QLatin1String("about:blank")))
                return;
            adblockSmokeStage = 3;
            // WebView::loadFinished -> AdBlockPage::applyRulesToPage
            // queued its style-injection runJavaScript before this
            // check runs, so the element must already exist.
            view->webPage()->runJavaScript(
                QLatin1String("!!document.getElementById('arora-adblock')"),
                [&application](const QVariant &result) {
                    const bool pass = result.toBool();
                    qInfo() << "adblock-smoke: cosmetic injection"
                            << (pass ? "PASS" : "FAIL");
                    application.exit(pass ? 0 : 1);
                });
        });
        QTimer::singleShot(20000, &application, [&application]() {
            qInfo() << "adblock-smoke: FAIL (timeout)";
            application.exit(1);
        });
        view->loadUrl(QUrl(QLatin1String("http://adblock-smoke.invalid/")));
    }

    // Headless verification for ADB01: subscribe to a real filter list
    // (default: live EasyList over https), let AdBlockSubscription
    // download it through the app-side NetworkAccessManager, then check
    // that the parsed rules actually reach the IO-thread matcher —
    // including a URL derived from a rule taken out of the downloaded
    // list itself — alongside a second (custom) subscription.  Exits 0
    // on PASS.
    const int listSmokeIndex = args.indexOf(QLatin1String("--adblock-list-smoke"));
    if (listSmokeIndex != -1) {
        QUrl listUrl(QStringLiteral("https://easylist.to/easylist/easylist.txt"));
        if (args.count() > listSmokeIndex + 1
            && !args.at(listSmokeIndex + 1).startsWith(QLatin1Char('-')))
            listUrl = QUrl(args.at(listSmokeIndex + 1));

        AdBlockManager *manager = AdBlockManager::instance();
        AdBlockNetwork *network = manager->network();

        // A second subscription proves multi-subscription matching.
        AdBlockSubscription *custom = manager->customRules();
        custom->addRule(AdBlockRule(QLatin1String("||list-smoke.invalid^")));

        QUrl subscribeUrl;
        subscribeUrl.setScheme(QLatin1String("abp"));
        subscribeUrl.setPath(QLatin1String("subscribe"));
        QUrlQuery subscribeQuery;
        subscribeQuery.addQueryItem(QLatin1String("location"),
                                    QString::fromUtf8(listUrl.toEncoded()));
        subscribeQuery.addQueryItem(QLatin1String("title"),
                                    QStringLiteral("list-smoke"));
        subscribeUrl.setQuery(subscribeQuery);
        AdBlockSubscription *subscription =
            new AdBlockSubscription(subscribeUrl, manager);
        manager->addSubscription(subscription);

        QObject::connect(subscription, &AdBlockSubscription::rulesChanged,
                         &application,
                         [&application, subscription, network]() {
            const QList<AdBlockRule> rules = subscription->allRules();
            int cosmetic = 0;
            int probes = 0;
            QUrl derivedUrl;
            bool foundRule = false;
            for (const AdBlockRule &rule : rules) {
                if (!rule.isEnabled())
                    continue;
                if (rule.isCSSRule()) {
                    ++cosmetic;
                    continue;
                }
                if (foundRule || probes >= 25 || rule.isException()
                    || !rule.isSupported())
                    continue;
                ++probes;
                // Turn "||host^..." / substring patterns into a probe
                // URL the rule must match.
                QString pattern = rule.filter();
                const int dollar = pattern.indexOf(QLatin1Char('$'));
                if (dollar != -1)
                    pattern = pattern.left(dollar);
                pattern.remove(QLatin1Char('|')).remove(QLatin1Char('^'))
                    .remove(QLatin1Char('*'));
                if (pattern.size() < 4)
                    continue;
                if (!pattern.contains(QLatin1Char('/'))
                    && pattern.contains(QLatin1Char('.'))) {
                    derivedUrl = QUrl(QLatin1String("http://")
                                      + pattern + QLatin1Char('/'));
                } else {
                    derivedUrl = QUrl(QLatin1String("http://example.com/")
                                      + pattern);
                }
                if (network->match(derivedUrl).action
                    != AdBlockDecision::Allow)
                    foundRule = true;
            }
            const bool customOk = network->shouldBlock(
                QUrl(QLatin1String("http://list-smoke.invalid/x")));
            const bool pass = rules.count() > 5000 && cosmetic > 0
                && foundRule && customOk;
            qInfo() << "adblock-list-smoke:" << (pass ? "PASS" : "FAIL")
                    << "rules:" << rules.count() << "cosmetic:" << cosmetic
                    << "derived:" << derivedUrl << "found:" << foundRule
                    << "custom:" << customOk;
            application.exit(pass ? 0 : 1);
        });
        QTimer::singleShot(90000, &application, [&application]() {
            qInfo() << "adblock-list-smoke: FAIL (timeout)";
            application.exit(1);
        });
    }

    // Headless verification for MIG10: stored form data is filled into
    // a loaded page by the injected autofill.js; a submit is reported
    // back through the aroraAutofill channel object and merged into the
    // store; an off-the-record page is still filled (old private-mode
    // parity) but must never be captured; and the store round-trips
    // through autofill.dat.  Exits 0 on PASS for all stages.
    // Same lifetime reason as adblockSmokeStage above — the lambdas
    // fire inside exec() after this if-block has closed.
    int autofillSmokeStage = 0;
    if (args.contains(QLatin1String("--autofill-smoke"))) {
        AutoFillManager *autoFill = AutoFillManager::instance();

        const QString fixturePath = QDir::temp().filePath(
            QLatin1String("arora-autofill-smoke.html"));
        QFile fixture(fixturePath);
        if (!fixture.open(QIODevice::WriteOnly)) {
            qInfo() << "autofill-smoke: FAIL (cannot write fixture)";
            return 1;
        }
        fixture.write("<html><body><form name=\"login\""
                      " onsubmit=\"return false\">"
                      "<input id=\"u\" name=\"user\" type=\"text\">"
                      "<input id=\"p\" name=\"pass\" type=\"password\">"
                      "<input type=\"submit\" value=\"go\">"
                      "</form></body></html>");
        fixture.close();
        const QUrl fixtureUrl = QUrl::fromLocalFile(fixturePath);

        // A stored entry for the fixture url means later captures take
        // the replace path and never hit the interactive
        // save-password prompt (which cannot be answered offscreen).
        AutoFillManager::Form seed;
        seed.url = fixtureUrl;
        seed.name = QLatin1String("login");
        seed.hasAPassword = true;
        seed.elements
            << qMakePair(QStringLiteral("user"), QStringLiteral("seeduser"))
            << qMakePair(QStringLiteral("pass"), QStringLiteral("seedpass"));
        autoFill->setForms(QList<AutoFillManager::Form>() << seed);

        auto hasElement = [autoFill](const QString &name, const QString &value) {
            const QList<AutoFillManager::Form> forms = autoFill->forms();
            for (const AutoFillManager::Form &form : forms)
                for (const AutoFillManager::Element &element : form.elements)
                    if (element.first == name && element.second == value)
                        return true;
            return false;
        };

        // Capture on the main (persistent) profile: a requestSubmit()
        // fires the submit event, the injected listener serializes the
        // form and reports it through the aroraAutofill bridge, which
        // replaces the seeded entry via autoFillChanged.
        QObject::connect(autoFill, &AutoFillManager::autoFillChanged,
                         &application,
                         [&application, autoFill, hasElement,
                          &autofillSmokeStage]() {
            if (autofillSmokeStage != 1)
                return;
            const bool pass = autoFill->forms().count() == 1
                && hasElement(QLatin1String("user"), QLatin1String("newuser"))
                && hasElement(QLatin1String("pass"), QLatin1String("newpass"));
            qInfo() << "autofill-smoke: capture" << (pass ? "PASS" : "FAIL")
                    << "forms:" << autoFill->forms().count();
            if (!pass) {
                application.exit(1);
                return;
            }
            autofillSmokeStage = 2;

            // Off-the-record profile: fill still applies (parity with
            // the old global private mode) but the bridge must drop
            // every submit report.
            QWebEngineProfile *otrProfile = new QWebEngineProfile(&application);
            WebView *otrView = new WebView(otrProfile);
            otrView->setAttribute(Qt::WA_DeleteOnClose);
            otrView->show();
            const QUrl fixtureUrl =
                autoFill->forms().first().url; // same fixture page
            QObject::connect(otrView, &QWebEngineView::loadFinished,
                             &application,
                             [&application, otrView, hasElement, fixtureUrl](bool ok) {
                if (!ok || otrView->url() != fixtureUrl)
                    return;
                otrView->webPage()->runJavaScript(
                    QLatin1String("document.getElementById('u').value"),
                    [&application, otrView, hasElement](const QVariant &result) {
                    // The stored entry holds the values captured in the
                    // previous stage (the seed was replaced).
                    const bool filled =
                        result.toString() == QLatin1String("newuser");
                    qInfo() << "autofill-smoke: otr fill"
                            << (filled ? "PASS" : "FAIL");
                    if (!filled) {
                        delete otrView;
                        application.exit(1);
                        return;
                    }
                    // Give the (supposedly absent) capture hook no
                    // chance: submit and check nothing was stored.
                    otrView->webPage()->runJavaScript(QLatin1String(
                        "document.getElementById('u').value='otruser';"
                        "document.getElementById('p').value='otrpass';"
                        "document.forms[0].requestSubmit();"));
                    QTimer::singleShot(2000, &application,
                                       [&application, hasElement,
                                        otrView]() {
                        const bool pass = !hasElement(
                            QLatin1String("user"), QLatin1String("otruser"));
                        qInfo() << "autofill-smoke: otr capture dropped"
                                << (pass ? "PASS" : "FAIL");
                        if (!pass) {
                            delete otrView;
                            application.exit(1);
                            return;
                        }
                        // autofill.dat round-trip through a fresh
                        // manager reading the same data dir.
                        AutoFillManager *autoFill =
                            AutoFillManager::instance();
                        QMetaObject::invokeMethod(autoFill, "save",
                                                  Qt::DirectConnection);
                        AutoFillManager probe(&application);
                        bool stored = false;
                        for (const AutoFillManager::Form &form : probe.forms())
                            for (const AutoFillManager::Element &e : form.elements)
                                stored |= (e.first == QLatin1String("user")
                                           && e.second == QLatin1String("newuser"));
                        qInfo() << "autofill-smoke: persistence"
                                << (stored ? "PASS" : "FAIL")
                                << "forms:" << probe.forms().count();

                        // SEC03 at-rest checks: autofill.dat must be a
                        // sealed SecureStore blob — magic header, no
                        // plaintext credential bytes — and the seal
                        // must round-trip and reject tampering.
                        bool sealed = false;
                        {
                            QFile storeFile(BrowserPaths::dataFilePath(
                                QLatin1String("autofill.dat")));
                            if (storeFile.open(QIODevice::ReadOnly)) {
                                const QByteArray raw =
                                    storeFile.readAll();
                                sealed = SecureStore::isSealed(raw)
                                    && !raw.contains("newpass")
                                    && !raw.contains("newuser");
                            }
                        }
                        qInfo() << "autofill-smoke: sealed-at-rest"
                                << (sealed ? "PASS" : "FAIL");

                        bool crypto = sealed;
                        if (sealed) {
                            const QByteArray blob =
                                SecureStore::seal("round-trip");
                            bool ok = false;
                            crypto = !blob.isEmpty()
                                && SecureStore::open(blob, &ok)
                                    == "round-trip" && ok;
                            QByteArray tampered = blob;
                            tampered[tampered.size() - 1] =
                                tampered[tampered.size() - 1] ^ 0xff;
                            bool tamperOk = true;
                            SecureStore::open(tampered, &tamperOk);
                            bool strOk = false;
                            const QString str = SecureStore::openString(
                                SecureStore::sealString(
                                    QStringLiteral("p@ss")), &strOk);
                            crypto = crypto && !tamperOk
                                && strOk && str == QLatin1String("p@ss");
                            qInfo() << "autofill-smoke: securestore"
                                    << (crypto ? "PASS" : "FAIL");
                        }

                        // Delete the OTR view before exit — its page
                        // must die before the OTR profile is released.
                        delete otrView;
                        application.exit(stored && sealed && crypto
                                         ? 0 : 1);
                    });
                });
            });
            otrView->loadUrl(fixtureUrl);
        });

        QObject::connect(view, &QWebEngineView::loadFinished, &application,
                         [view, &application, fixtureUrl,
                          &autofillSmokeStage](bool ok) {
            if (autofillSmokeStage != 0 || !ok || view->url() != fixtureUrl)
                return;
            autofillSmokeStage = 1;
            // Fill check, then rewrite the fields and submit once the
            // channel handshake has had time to install the listener.
            view->webPage()->runJavaScript(
                QLatin1String("document.getElementById('u').value"),
                [&application, view](const QVariant &result) {
                const bool filled =
                    result.toString() == QLatin1String("seeduser");
                qInfo() << "autofill-smoke: fill" << (filled ? "PASS" : "FAIL")
                        << "got:" << result.toString();
                if (!filled) {
                    application.exit(1);
                    return;
                }
                QTimer::singleShot(700, &application, [view]() {
                    view->webPage()->runJavaScript(QLatin1String(
                        "document.getElementById('u').value='newuser';"
                        "document.getElementById('p').value='newpass';"
                        "document.forms[0].requestSubmit();"));
                });
            });
        });
        QTimer::singleShot(30000, &application, [&application]() {
            qInfo() << "autofill-smoke: FAIL (timeout)";
            application.exit(1);
        });
        view->loadUrl(fixtureUrl);
    }

    // ADB02 coverage comparison (CONFIG+=adblock_rust builds only):
    // feed a fixed corpus through the subscriptions and diff the
    // adblock-rust engine's decisions against both the native matcher
    // and the expected outcome.  Decisions normalize to
    // 0=allow 1=block 2=stub-redirect 3=url-rewrite ($removeparam).
    if (args.contains(QLatin1String("--adblock-rust-smoke"))) {
#if defined(ARORA_ADBLOCK_RUST)
        AdBlockManager *manager = AdBlockManager::instance();
        AdBlockSubscription *custom = manager->customRules();
        // Test-mode app data persists between runs — an earlier
        // --adblock-list-smoke leaves a full EasyList subscription
        // behind.  Disable everything but the custom corpus so the
        // probes are deterministic.
        for (AdBlockSubscription *s : manager->subscriptions()) {
            if (s != custom)
                s->setEnabled(false);
        }
        const char *corpus[] = {
            "||ads.example.com^",
            "||banner.example^$script",
            "@@||banner.example^$script,domain=trusted.example",
            "||tracker.example^$third-party",
            "||cdn.example/lib.js$~third-party",
            "||redir.example/vast.xml$redirect=noop-vast-4.0",
            "||param.example^$removeparam=utm_source",
            "smoke.example##.ad-banner",
        };
        for (const char *rule : corpus)
            custom->addRule(AdBlockRule(QLatin1String(rule)));

        AdBlockNetwork *network = manager->network();
        struct Probe {
            const char *url;
            const char *firstParty;
            int resourceType;
            int expected;
        };
        const Probe probes[] = {
            { "http://ads.example.com/a.js", "http://site.example/", 3, 1 },
            { "http://sub.ads.example.com/a", "http://site.example/", 3, 1 },
            { "http://other.example/ads.example.com.js",
              "http://site.example/", 3, 0 },
            { "http://banner.example/b.js", "http://site.example/", 3, 1 },
            { "http://banner.example/b.js", "http://trusted.example/", 3, 0 },
            { "http://banner.example/b.png", "http://site.example/", 4, 0 },
            { "http://tracker.example/t.js", "http://site.example/", 3, 1 },
            { "http://tracker.example/t.js",
              "http://tracker.example/", 3, 0 },
            { "http://cdn.example/lib.js", "http://cdn.example/", 3, 1 },
            { "http://cdn.example/lib.js", "http://site.example/", 3, 0 },
            { "http://redir.example/vast.xml", "http://site.example/", 13, 2 },
            { "http://param.example/x?utm_source=a&keep=1",
              "http://site.example/", 0, 3 },
            { "http://innocent.example/x.js", "http://site.example/", 3, 0 },
        };

        int rustExpected = 0;
        int agree = 0;
        const int total = int(sizeof(probes) / sizeof(probes[0]));
        for (const Probe &probe : probes) {
            const QUrl url(QLatin1String(probe.url));
            const QUrl firstParty(QLatin1String(probe.firstParty));
            const int native = adblockDecisionKind(
                network->matchNative(url, firstParty, probe.resourceType));
            const int rust = adblockDecisionKind(
                network->match(url, firstParty, probe.resourceType));
            agree += (native == rust);
            rustExpected += (rust == probe.expected);
            if (native != rust || rust != probe.expected)
                qInfo() << "adblock-rust-smoke: diff" << probe.url
                        << "native" << native << "rust" << rust
                        << "expected" << probe.expected;
        }

        const QJsonObject cosmetic = network->rustCosmetic(
            QUrl(QLatin1String("http://smoke.example/")));
        const bool cosmeticOk = cosmetic.value(QLatin1String("hide"))
            .toArray().contains(QLatin1String(".ad-banner"));

        const bool pass = rustExpected == total && cosmeticOk;
        qInfo() << "adblock-rust-smoke:" << (pass ? "PASS" : "FAIL")
                << "rust-matches-expected" << rustExpected << "/" << total
                << "native-agrees" << agree << "/" << total
                << "cosmetic" << cosmeticOk;
        return pass ? 0 : 1;
#else
        qInfo() << "adblock-rust-smoke: SKIP"
                << "(built without CONFIG+=adblock_rust)";
        return 0;
#endif
    }

    // Headless verification for MIG11: the settings dialog's
    // websettings map must land on the profile's QWebEngineSettings
    // (fonts, WebAttribute toggles, user style sheet injected as a
    // QWebEngineScript — setUserStyleSheetUrl is gone), the cookie and
    // network groups must reach the profile cookie jar and http cache
    // settings, the shared Accept-Language helpers must emit a valid
    // header, and ClearPrivateData must wipe history, cookies and the
    // icon cache.  Exits 0 on PASS.
    if (args.contains(QLatin1String("--settings-smoke"))) {
        bool ok = true;
        const auto check = [&ok](bool condition, const char *what) {
            if (!condition)
                qInfo() << "settings-smoke: FAIL at" << what;
            ok = ok && condition;
        };

        const QString cssPath = QDir::temp().filePath(
            QLatin1String("arora-settings-smoke.css"));
        {
            QFile css(cssPath);
            if (css.open(QIODevice::WriteOnly))
                css.write("body { background: red; }");
        }

        SettingsDialog dialog;
        dialog.enableJavascript->setChecked(false);
        dialog.enableImages->setChecked(false);
        dialog.acceptCombo->setCurrentIndex(1);   // CookieJar::AcceptNever
        dialog.networkCache->setChecked(false);
        dialog.userStyleSheet->setText(cssPath);
        dialog.accept();

        QWebEngineSettings *engineSettings = profile->settings();
        check(!engineSettings->testAttribute(QWebEngineSettings::JavascriptEnabled),
              "enableJavascript");
        check(!engineSettings->testAttribute(QWebEngineSettings::AutoLoadImages),
              "enableImages");
        check(profile->httpCacheType() == QWebEngineProfile::NoCache,
              "httpCacheType");
        check(cookieJar->acceptPolicy() == CookieJar::AcceptNever,
              "acceptPolicy");
        check(!profile->httpAcceptLanguage().isEmpty(),
              "httpAcceptLanguage");
        bool foundStyleScript = false;
        const QList<QWebEngineScript> scripts = profile->scripts()->toList();
        for (const QWebEngineScript &script : scripts)
            foundStyleScript |= script.name() == QLatin1String("aroraUserStyleSheet");
        check(foundStyleScript, "userStyleSheet script");

        check(AcceptLanguageDialog::httpString(
                  QStringList() << QLatin1String("English (United States) [en-us]")
                                << QLatin1String("French [fr]"))
              == "en-us, fr;q=0.9", "httpString");
        check(!AcceptLanguageDialog::acceptLanguages().isEmpty(),
              "acceptLanguages");

        // ClearPrivateData: seed history, an icon and a cookie, then
        // clear and verify everything is gone.
        HistoryManager *history = HistoryManager::instance();
        const QUrl seededUrl(QLatin1String("http://settings-smoke.example/"));
        history->addHistoryEntry(seededUrl.toString());
        const QIcon seededIcon(QPixmap(4, 4));
        history->setIcon(seededUrl, seededIcon);
        cookieJar->setAcceptPolicy(CookieJar::AcceptAlways);
        cookieJar->setCookiesFromUrl(
            QList<QNetworkCookie>() << QNetworkCookie("smoke", "1"),
            seededUrl);

        ClearPrivateData clearDialog;
        clearDialog.accept();

        check(!history->historyContains(seededUrl.toString()),
              "browsing history cleared");
        check(cookieJar->cookies().isEmpty(), "cookies cleared");
        check(history->icon(seededUrl).cacheKey() != seededIcon.cacheKey(),
              "icons cleared");

        // Leave no residue: drop the keys the dialog wrote and
        // re-apply defaults (also exercises the reset path).
        QSettings().clear();
        BrowserProfile::applySettings(profile);
        QFile::remove(cssPath);

        qInfo() << "settings-smoke:" << (ok ? "PASS" : "FAIL");
        return ok ? 0 : 1;
    }

    // Headless verification for MIG12: the in-page find bar drives
    // QWebEngineView::findText — forward/backward wrap freely
    // (WebEngine always wraps, the FindWrapsAroundDocument flag is
    // gone), a miss sets the "Not Found" info label, and the
    // Highlight-All toggle reduces to re-find/clear since WebEngine
    // highlights every match anyway.  The render-side selection is
    // read back through window.getSelection().  Exits 0 on PASS.
    if (args.contains(QLatin1String("--find-smoke"))) {
        const QString fixturePath = QDir::temp().filePath(
            QLatin1String("arora-find-smoke.html"));
        {
            QFile fixture(fixturePath);
            if (!fixture.open(QIODevice::WriteOnly)) {
                qInfo() << "find-smoke: FAIL (cannot write fixture)";
                return 1;
            }
            fixture.write("<html><body><p>needle one</p>"
                          "<p>haystack</p><p>needle two</p></body></html>");
        }
        const QUrl fixtureUrl = QUrl::fromLocalFile(fixturePath);

        // A leftover ##body cosmetic rule in the shared test-mode
        // settings hides the whole page (display:none text is
        // unfindable); suspend adblocking for the duration and put the
        // persisted flag back on the way out.
        AdBlockManager *adblock = AdBlockManager::instance();
        const bool adblockWasEnabled = adblock->isEnabled();
        adblock->setEnabled(false);
        QObject::connect(&application, &QCoreApplication::aboutToQuit,
                         &application, [adblock, adblockWasEnabled]() {
            adblock->setEnabled(adblockWasEnabled);
        });

        WebViewSearch *searchBar = new WebViewSearch(view, &window);
        QLineEdit *searchEdit =
            searchBar->findChild<QLineEdit*>(QLatin1String("searchLineEdit"));
        QLabel *searchInfo =
            searchBar->findChild<QLabel*>(QLatin1String("searchInfo"));
        QToolButton *highlightAll =
            searchBar->findChild<QToolButton*>(QLatin1String("highlightAllButton"));
        if (!searchEdit || !searchInfo || !highlightAll) {
            qInfo() << "find-smoke: FAIL (search bar widgets missing)";
            return 1;
        }

        // findText answers asynchronously; every reply lands here.
        // (The find highlight is renderer-internal — window.getSelection()
        // does not observe it — so the result object is the readback.)
        auto resultsSeen = std::make_shared<int>(0);
        auto lastMatches = std::make_shared<int>(-1);
        auto lastActive = std::make_shared<int>(-1);
        QObject::connect(view->webPage(), &QWebEnginePage::findTextFinished,
                         &application,
                         [resultsSeen, lastMatches, lastActive]
                         (const QWebEngineFindTextResult &result) {
            *lastMatches = result.numberOfMatches();
            *lastActive = result.activeMatch();
            ++*resultsSeen;
        });

        // Runs ready() once a findText reply newer than 'before' has
        // arrived (or after ~5s — the check then fails on stale data).
        auto awaitResult = [resultsSeen](int before,
                                         std::function<void()> ready) {
            auto ticks = std::make_shared<int>(0);
            QTimer *poll = new QTimer(qApp);
            QObject::connect(poll, &QTimer::timeout, qApp,
                [resultsSeen, before, ready, ticks, poll]() {
                if (*resultsSeen > before || ++*ticks > 100) {
                    poll->stop();
                    poll->deleteLater();
                    ready();
                }
            });
            poll->start(50);
        };
        // A short grace period after each reply lets the search bar's
        // own callback (which owns the info label) settle.
        auto settle = [](std::function<void()> fn) {
            QTimer::singleShot(200, qApp, [fn]() { fn(); });
        };

        QObject::connect(view, &QWebEngineView::loadFinished, &application,
            [view, fixtureUrl, fixturePath, searchBar, searchEdit,
             searchInfo, highlightAll, resultsSeen, lastMatches, lastActive,
             awaitResult, settle](bool ok) {
            if (!ok || view->url() != fixtureUrl)
                return;

            // Stage 1: forward find selects the first of two matches.
            searchEdit->setText(QLatin1String("needle"));
            const int base1 = *resultsSeen;
            searchBar->findNext();
            awaitResult(base1, [=]() {
                settle([=]() {
                const bool pass = *lastMatches == 2 && *lastActive >= 0
                    && searchInfo->text().isEmpty();
                qInfo() << "find-smoke: next" << (pass ? "PASS" : "FAIL")
                        << "matches:" << *lastMatches
                        << "active:" << *lastActive;
                if (!pass) { qApp->exit(1); return; }
                const int active1 = *lastActive;

                // Stage 2: forward again moves to the second match.
                const int base2 = *resultsSeen;
                searchBar->findNext();
                awaitResult(base2, [=]() {
                settle([=]() {
                const bool pass = *lastMatches == 2
                    && *lastActive != active1;
                qInfo() << "find-smoke: next-wrap"
                        << (pass ? "PASS" : "FAIL")
                        << "active:" << *lastActive;
                if (!pass) { qApp->exit(1); return; }

                // Stage 3: backward returns to the first match.
                const int base3 = *resultsSeen;
                searchBar->findPrevious();
                awaitResult(base3, [=]() {
                settle([=]() {
                const bool pass = *lastMatches == 2
                    && *lastActive == active1;
                qInfo() << "find-smoke: previous"
                        << (pass ? "PASS" : "FAIL")
                        << "active:" << *lastActive;
                if (!pass) { qApp->exit(1); return; }

                // Stage 4: a miss reports Not Found on the info label.
                searchEdit->setText(QLatin1String("zzz-absent"));
                const int base4 = *resultsSeen;
                searchBar->findNext();
                awaitResult(base4, [=]() {
                settle([=]() {
                const bool pass = *lastMatches == 0
                    && !searchInfo->text().isEmpty();
                qInfo() << "find-smoke: not-found"
                        << (pass ? "PASS" : "FAIL")
                        << "info:" << searchInfo->text();
                if (!pass) { qApp->exit(1); return; }

                // Stage 5: toggling Highlight-All on re-runs the find
                // (WebEngine always highlights every match).
                searchEdit->setText(QLatin1String("needle"));
                const int base5 = *resultsSeen;
                highlightAll->setChecked(true);
                awaitResult(base5, [=]() {
                const bool pass = *lastMatches == 2;
                qInfo() << "find-smoke: highlight-all"
                        << (pass ? "PASS" : "FAIL")
                        << "matches:" << *lastMatches;
                if (!pass) { qApp->exit(1); return; }

                // Stage 6: toggling off clears the find (no reply is
                // emitted for an empty needle — verify the next find
                // still works afterwards).
                highlightAll->setChecked(false);
                const int base6 = *resultsSeen;
                searchBar->findNext();
                awaitResult(base6, [=]() {
                const bool pass = *lastMatches == 2;
                qInfo() << "find-smoke:"
                        << (pass ? "PASS" : "FAIL")
                        << "(refind-after-clear:" << pass << ")";
                QFile::remove(fixturePath);
                qApp->exit(pass ? 0 : 1);
                });
                });
                });
                });
                });
                });
                });
                });
                });
            });
        });
        QTimer::singleShot(20000, &application, []() {
            qInfo() << "find-smoke: FAIL (timeout)";
            qApp->exit(1);
        });
        view->loadUrl(fixtureUrl);
    }

    // Headless verification for MIG12 (view source): the viewer
    // re-fetches the page through the app-side NAM and shows the raw
    // wire bytes when they parse to the same DOM the page serialized
    // — otherwise the DOM dump is shown.  A second viewer fed a bogus
    // dump exercises the fallback branch.  The syntax highlighter is
    // checked on a standalone document (no renderer needed) and the
    // in-viewer find bar exercises PlainTextEditSearch.  Exits 0 on
    // PASS.
    if (args.contains(QLatin1String("--source-smoke"))) {
        const QString fixturePath = QDir::temp().filePath(
            QLatin1String("arora-source-smoke.html"));
        const QByteArray bytes =
            "<html><!--c--><body><p class=\"x\">needle &amp; more</p>"
            "</body></html>";
        {
            QFile fixture(fixturePath);
            if (!fixture.open(QIODevice::WriteOnly)) {
                qInfo() << "source-smoke: FAIL (cannot write fixture)";
                return 1;
            }
            fixture.write(bytes);
        }
        const QUrl fixtureUrl = QUrl::fromLocalFile(fixturePath);

        // Cosmetic adblock rules would be injected into the serialized
        // DOM the viewer compares against (see --find-smoke); suspend
        // adblocking for the duration.
        AdBlockManager *adblock = AdBlockManager::instance();
        const bool adblockWasEnabled = adblock->isEnabled();
        adblock->setEnabled(false);
        QObject::connect(&application, &QCoreApplication::aboutToQuit,
                         &application, [adblock, adblockWasEnabled]() {
            adblock->setEnabled(adblockWasEnabled);
        });

        // The ported QRegularExpression state machine must mark up a
        // document without any WebEngine involvement.
        QTextDocument document;
        SourceHighlighter highlighter(&document);
        document.setPlainText(QString::fromUtf8(bytes));
        // Highlighting lands in the block layout, which is computed lazily.
        document.documentLayout()->documentSize();
        if (document.firstBlock().layout()->formats().isEmpty()) {
            qInfo() << "source-smoke: FAIL (highlighter produced no formats)";
            return 1;
        }
        qInfo() << "source-smoke: highlighter PASS";

        auto pending = std::make_shared<int>(0);
        auto failures = std::make_shared<int>(0);
        auto finish = [&application, pending, failures,
                       fixturePath](bool ok) {
            *failures += ok ? 0 : 1;
            if (--*pending == 0) {
                qInfo() << "source-smoke:"
                        << (*failures == 0 ? "PASS" : "FAIL");
                QFile::remove(fixturePath);
                application.exit(*failures == 0 ? 0 : 1);
            }
        };

        // Waits out the re-fetch + probe-page comparison, then checks
        // the shown text and drives the viewer's find bar.
        auto checkViewer = [finish](SourceViewer *viewer,
                                    const QString &expected,
                                    const char *what) {
            QPlainTextEdit *edit = viewer->findChild<QPlainTextEdit*>();
            PlainTextEditSearch *search =
                viewer->findChild<PlainTextEditSearch*>();
            QLineEdit *searchEdit = search
                ? search->findChild<QLineEdit*>(QLatin1String("searchLineEdit"))
                : nullptr;
            if (!edit || !search || !searchEdit) {
                finish(false);
                return;
            }
            auto ticks = std::make_shared<int>(0);
            QTimer *poll = new QTimer(viewer);
            QObject::connect(poll, &QTimer::timeout, viewer,
                [edit, search, searchEdit, expected, what,
                 ticks, poll, finish]() {
                if (edit->toPlainText() == QLatin1String("Loading...")) {
                    if (++*ticks > 100) {
                        poll->stop();
                        qInfo() << "source-smoke:" << what
                                << "FAIL (probe timeout)";
                        finish(false);
                    }
                    return;
                }
                poll->stop();
                const bool contentOk = edit->toPlainText() == expected;
                if (!contentOk)
                    qInfo() << "source-smoke:" << what << "shown was:"
                            << edit->toPlainText().left(200)
                            << "| expected:" << expected.left(200);
                searchEdit->setText(QLatin1String("needle"));
                search->findNext();
                const bool findOk =
                    !expected.contains(QLatin1String("needle"))
                    || edit->textCursor().selectedText()
                           == QLatin1String("needle");
                qInfo() << "source-smoke:" << what
                        << (contentOk && findOk ? "PASS" : "FAIL")
                        << "(content:" << contentOk << "find:" << findOk << ")";
                finish(contentOk && findOk);
            });
            poll->start(100);
        };

        QObject::connect(view, &QWebEngineView::loadFinished, &application,
            [view, fixtureUrl, bytes, pending, checkViewer](bool ok) {
            if (!ok || view->url() != fixtureUrl)
                return;
            // Serialize the loaded DOM exactly like
            // BrowserMainWindow::viewPageSource() does.
            view->webPage()->toHtml(
                [view, fixtureUrl, bytes, pending, checkViewer](const QString &markup) {
                // The faithful DOM dump: the probe must judge the raw
                // bytes equivalent and show them verbatim.
                *pending += 2;
                checkViewer(new SourceViewer(markup, view->title(),
                                             fixtureUrl, view),
                            QString::fromUtf8(bytes), "raw");
                // A mismatched dump must be shown as-is (probe's
                // toHtml never equals it).
                checkViewer(new SourceViewer(QLatin1String("bogus-dom-dump"),
                                             view->title(), fixtureUrl, view),
                            QLatin1String("bogus-dom-dump"), "fallback");
            });
        });
        QTimer::singleShot(30000, &application, [&application]() {
            qInfo() << "source-smoke: FAIL (timeout)";
            application.exit(1);
        });
        view->loadUrl(fixtureUrl);
    }

    // Headless verification for MIG14: a real BrowserMainWindow must
    // construct with one tab, gain a second through the window-level
    // new-tab action, load a file:// page and propagate its title to
    // the window title, and route a script-driven window.open()
    // through WebPage::createWindow -> TabWidget::getView into a third
    // tab.  The harness runs a plain QApplication, so
    // BrowserApplication::instance() is null and the chrome must
    // degrade gracefully.  Exits 0 on PASS.
    if (args.contains(QLatin1String("--browser-smoke"))) {
        // A leftover ##body cosmetic rule from earlier test-mode runs
        // would restyle the page; suspend adblock for determinism.
        AdBlockManager::instance()->setEnabled(false);

        // The window is deleted before application.exit() below: an
        // unregistered window would otherwise outlive the profile at
        // teardown and crash inside QtWebEngine's shutdown.
        BrowserMainWindow *browserWindow = new BrowserMainWindow();
        browserWindow->show();
        TabWidget *tabWidget = browserWindow->tabWidget();

        bool ok = tabWidget && tabWidget->count() == 1
            && tabWidget->currentWebView()
            && browserWindow->toolbarSearch()
            && browserWindow->menuBar();
        if (!ok) {
            qInfo() << "browser-smoke: FAIL (window construction)";
            return 1;
        }
        qInfo() << "browser-smoke: window PASS (1 tab)";

        tabWidget->newTabAction()->trigger();
        if (tabWidget->count() != 2) {
            qInfo() << "browser-smoke: FAIL (newTabAction)"
                    << "tabs:" << tabWidget->count();
            return 1;
        }
        qInfo() << "browser-smoke: new-tab action PASS (2 tabs)";

        const QString fixturePath = QDir::temp().filePath(
            QLatin1String("arora-browser-smoke.html"));
        {
            QFile fixture(fixturePath);
            if (!fixture.open(QIODevice::WriteOnly)) {
                qInfo() << "browser-smoke: FAIL (cannot write fixture)";
                return 1;
            }
            fixture.write("<html><head><title>browser-smoke-page</title>"
                          "</head><body>chrome"
                          "<a id=\"l\" href=\"about:blank\" target=\"_blank\">x</a>"
                          "</body></html>");
        }
        const QUrl fixtureUrl = QUrl::fromLocalFile(fixturePath);
        WebView *firstTab = tabWidget->webView(0);
        tabWidget->setCurrentIndex(0);

        QObject::connect(firstTab, &QWebEngineView::loadFinished,
                         &application,
                         [&application, browserWindow, tabWidget, firstTab,
                          fixtureUrl, fixturePath](bool ok) {
            if (!ok || firstTab->url() != fixtureUrl)
                return;
            const bool titleOk = browserWindow->windowTitle()
                .contains(QLatin1String("browser-smoke-page"));
            qInfo() << "browser-smoke: load+title"
                    << (titleOk ? "PASS" : "FAIL")
                    << "title:" << browserWindow->windowTitle();
            if (!titleOk) {
                QFile::remove(fixturePath);
                delete browserWindow;
                application.exit(1);
                return;
            }
            // A real click on the target=_blank link routes through
            // WebPage::createWindow -> TabWidget::getView and must grow
            // the tab strip to three tabs.  (Synthesized JS has no user
            // activation, so Chromium would block it as a popup.)
            firstTab->webPage()->runJavaScript(
                QLatin1String("JSON.stringify("
                              "document.getElementById('l').getBoundingClientRect())"),
                [tabWidget](const QVariant &rectVar) {
                const QJsonObject rect =
                    QJsonDocument::fromJson(rectVar.toString().toUtf8()).object();
                const QPointF pos(rect[QLatin1String("x")].toDouble()
                                      + rect[QLatin1String("width")].toDouble() / 2,
                                  rect[QLatin1String("y")].toDouble()
                                      + rect[QLatin1String("height")].toDouble() / 2);
                WebView *view = tabWidget->currentWebView();
                QWidget *proxy = view->focusProxy() ? view->focusProxy() : view;
                QMouseEvent press(QEvent::MouseButtonPress, pos,
                                  proxy->mapToGlobal(pos.toPoint()),
                                  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QMouseEvent release(QEvent::MouseButtonRelease, pos,
                                    proxy->mapToGlobal(pos.toPoint()),
                                    Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QCoreApplication::sendEvent(proxy, &press);
                QCoreApplication::sendEvent(proxy, &release);
            });
            QTimer::singleShot(3000, &application,
                               [&application, tabWidget, browserWindow,
                                fixturePath]() {
                const bool pass = tabWidget->count() == 3;
                qInfo() << "browser-smoke: target=_blank tab"
                        << (pass ? "PASS" : "FAIL")
                        << "tabs:" << tabWidget->count();
                qInfo() << "browser-smoke:" << (pass ? "PASS" : "FAIL");
                QFile::remove(fixturePath);
                delete browserWindow;
                application.exit(pass ? 0 : 1);
            });
        });
        QTimer::singleShot(20000, &application,
                           [&application, browserWindow]() {
            qInfo() << "browser-smoke: FAIL (timeout)";
            delete browserWindow;
            application.exit(1);
        });
        tabWidget->loadUrl(fixtureUrl, TabWidget::CurrentTab);
    }

    // Headless verification for EXT01: the QWebEngineExtensionManager
    // preview must drive a Manifest-V3 fixture through the whole
    // lifecycle — load (arrives disabled), enable, unload, install
    // into the profile's installPath, uninstall — while the manifest
    // inspector and the user-scripts QWebEngineScriptCollection path
    // are checked synchronously.  Exits 0 on PASS.
    // Function scope on purpose: the async lifecycle connects inside
    // the smoke capture [&] and run in exec() after its if-block has
    // closed — block-local state would dangle (ASan use-after-scope).
    ExtensionManager *extensions = nullptr;
    int failures = 0;
    const auto check = [&failures](bool ok, const char *what) {
        qInfo() << "extension-smoke:" << what << (ok ? "PASS" : "FAIL");
        if (!ok)
            ++failures;
    };
    const auto die = [&application](const QString &why) {
        qInfo() << "extension-smoke: FAIL" << why;
        application.exit(1);
    };
    QString extensionId;
    QString extDir;
    QTimer *enablePoll = nullptr;
    std::shared_ptr<int> pollTicks;
    if (args.contains(QLatin1String("--extension-smoke"))) {
        extensions = ExtensionManager::instance();

        check(ExtensionManager::isSupported(), "webengine_extensions feature");
        check(!extensions->extensions().isEmpty(),
              "built-in components listed");

        // Manifest-V3 fixture on disk.
        extDir = QDir::temp().filePath(
            QLatin1String("arora-ext-smoke"));
        QDir().mkpath(extDir);
        {
            QFile manifestFile(extDir + QLatin1String("/manifest.json"));
            if (!manifestFile.open(QIODevice::WriteOnly)) {
                qInfo() << "extension-smoke: FAIL (cannot write manifest)";
                return 1;
            }
            manifestFile.write(
                "{\"manifest_version\":3,"
                "\"name\":\"arora-smoke-ext\","
                "\"version\":\"0.1\","
                "\"description\":\"EXT01 smoke fixture\","
                "\"permissions\":[\"storage\",\"tabs\",\"nativeMessaging\"],"
                "\"host_permissions\":[\"https://*.example.com/*\"],"
                "\"action\":{\"default_title\":\"smoke\"},"
                "\"background\":{\"service_worker\":\"sw.js\"}}");
            QFile worker(extDir + QLatin1String("/sw.js"));
            if (!worker.open(QIODevice::WriteOnly)) {
                qInfo() << "extension-smoke: FAIL (cannot write worker)";
                return 1;
            }
            worker.write("chrome.runtime.onInstalled.addListener(function(){});\n");
        }

        // Synchronous checks: manifest inspector + permission
        // classification + the user-scripts script-collection path.
        const ExtensionManager::Manifest manifest =
            ExtensionManager::inspectManifest(extDir);
        check(manifest.valid && manifest.manifestVersion == 3
              && manifest.name == QLatin1String("arora-smoke-ext")
              && manifest.hasBackground && manifest.hasAction
              && manifest.permissions.size() == 3
              && manifest.hostPermissions.size() == 1,
              "manifest parsed");
        check(manifest.unsupported.contains(QLatin1String("tabs"))
              && manifest.unsupported.contains(QLatin1String("nativeMessaging"))
              && !manifest.unsupported.contains(QLatin1String("storage")),
              "unsupported chrome.* APIs flagged");
        const ExtensionManager::Manifest mv2 =
            ExtensionManager::inspectManifest(QString());
        check(!mv2.valid && !mv2.error.isEmpty(),
              "missing manifest reported");

        const QString scriptDir = ExtensionManager::userScriptsPath();
        const QString scriptPath = scriptDir
            + QLatin1String("/smoke-user.js");
        {
            QFile script(scriptPath);
            if (!script.open(QIODevice::WriteOnly)) {
                qInfo() << "extension-smoke: FAIL (cannot write user script)";
                return 1;
            }
            script.write("// smoke\n");
        }
        extensions->reloadUserScripts();
        bool scriptInstalled = false;
        const QList<QWebEngineScript> profileScripts = profile->scripts()->toList();
        for (const QWebEngineScript &script : profileScripts) {
            if (script.name() == QLatin1String("userscript:smoke-user.js"))
                scriptInstalled = true;
        }
        check(scriptInstalled, "user script injected into profile");
        check(extensions->userScriptNames().contains(QLatin1String("smoke-user.js")),
              "user script listed");
        QFile::remove(scriptPath);
        extensions->reloadUserScripts();

        // Async lifecycle driven by the manager's finished signals.
        extensionId.clear();
        enablePoll = new QTimer(&application);
        pollTicks = std::make_shared<int>(0);

        QObject::connect(extensions, &ExtensionManager::extensionLoaded,
            &application, [&](const ExtensionManager::ExtensionInfo &info) {
            if (info.name != QLatin1String("arora-smoke-ext"))
                return;
            check(info.loaded && info.error.isEmpty(),
                  "loadExtension finished");
            check(!info.enabled, "extension loads disabled");
            extensionId = info.id;
            extensions->setExtensionEnabled(extensionId, true);
            *pollTicks = 0;
            enablePoll->start(200);
        });

        QObject::connect(enablePoll, &QTimer::timeout, &application, [&]() {
            for (const ExtensionManager::ExtensionInfo &info
                 : extensions->extensions()) {
                if (info.id == extensionId && info.enabled) {
                    enablePoll->stop();
                    qInfo() << "extension-smoke: enable PASS";
                    // Loaded-but-not-installed removes via unload.
                    extensions->removeExtension(extensionId);
                    return;
                }
            }
            if (++*pollTicks > 25) {
                enablePoll->stop();
                die(QStringLiteral("setExtensionEnabled never applied"));
            }
        });

        QObject::connect(extensions, &ExtensionManager::extensionUnloaded,
            &application, [&](const ExtensionManager::ExtensionInfo &info) {
            if (info.id != extensionId)
                return;
            qInfo() << "extension-smoke: unload PASS";
            extensions->installExtension(extDir);
        });

        QObject::connect(extensions, &ExtensionManager::extensionInstalled,
            &application, [&](const ExtensionManager::ExtensionInfo &info) {
            if (info.name != QLatin1String("arora-smoke-ext")) {
                qInfo() << "extension-smoke: installFinished ignored"
                        << "name:" << info.name << "id:" << info.id
                        << "installed:" << info.installed
                        << "loaded:" << info.loaded
                        << "error:" << info.error;
                return;
            }
            check(info.installed && info.error.isEmpty(),
                  "installExtension finished");
            check(!extensions->installPath().isEmpty()
                  && info.path.startsWith(extensions->installPath()),
                  "install persisted under profile installPath");
            extensionId = info.id;
            extensions->removeExtension(extensionId);
        });

        QObject::connect(extensions, &ExtensionManager::extensionUninstalled,
            &application, [&](const ExtensionManager::ExtensionInfo &info) {
            if (info.id != extensionId)
                return;
            bool gone = true;
            for (const ExtensionManager::ExtensionInfo &rest
                 : extensions->extensions()) {
                if (rest.id == extensionId)
                    gone = false;
            }
            check(gone, "uninstall removes extension");
            qInfo() << "extension-smoke:"
                    << (failures == 0 ? "PASS" : "FAIL")
                    << "failures:" << failures;
            QDir(extDir).removeRecursively();
            application.exit(failures == 0 ? 0 : 1);
        });

        QObject::connect(extensions, &ExtensionManager::errorOccurred,
            &application, [die](const QString &message) {
            die(QStringLiteral("errorOccurred: %1").arg(message));
        });

        QTimer::singleShot(20000, &application, [die]() {
            die(QStringLiteral("timeout"));
        });
        extensions->loadExtension(extDir);

        // exec() must run while this block's locals are still alive:
        // the finished-signal lambdas above capture them by reference.
        return application.exec();
    }

    // Headless verification for UA01: the browsing profile must send a
    // vanilla Chrome UA — Qt's factory default minus the
    // "QtWebEngine/<ver>" product token Google's /sorry/ bot check
    // fingerprints — while the UserAgentMenu override still wins and
    // clearing it restores the vanilla UA on both the named and the
    // off-the-record private profile.  The refreshed useragents.xml
    // presets are sanity-checked too.  The live Google search at the
    // end is report-only: offline runs SKIP it and a /sorry/ landing
    // page means IP reputation, not the UA, tripped bot detection.
    // Exits 0 when all local checks PASS.
    if (args.contains(QLatin1String("--ua-smoke"))) {
        int failures = 0;
        const auto check = [&failures](bool ok, const char *what) {
            qInfo() << "ua-smoke:" << what << (ok ? "PASS" : "FAIL");
            if (!ok)
                ++failures;
        };

        const QString vanilla = BrowserProfile::defaultHttpUserAgent();
        check(!vanilla.isEmpty(), "default UA non-empty");
        check(!vanilla.contains(QLatin1String("QtWebEngine")),
              "no QtWebEngine token");
        check(vanilla.contains(QLatin1String("Chrome/"))
              && vanilla.contains(QLatin1String("Safari/")),
              "vanilla UA is Chrome-shaped");
        check(profile->httpUserAgent() == vanilla,
              "browsing profile sends vanilla UA");

        // The UserAgentMenu override wins; clearing it must restore
        // the vanilla UA (an empty string used to be written back).
        WebPage::setUserAgent(QLatin1String("smoke-ua/1.0"));
        check(profile->httpUserAgent() == QLatin1String("smoke-ua/1.0"),
              "override reaches browsing profile");
        WebPage::setUserAgent(QString());
        check(profile->httpUserAgent() == vanilla,
              "clearing override restores vanilla UA");

        // The off-the-record private profile gets the same treatment.
        QWebEngineProfile *otr = BrowserProfile::privateProfile();
        BrowserProfile::applySettings(otr);
        check(otr->httpUserAgent() == vanilla,
              "private profile sends vanilla UA");
        WebPage::setUserAgent(QLatin1String("smoke-ua/1.0"));
        check(otr->httpUserAgent() == QLatin1String("smoke-ua/1.0"),
              "override reaches private profile");
        WebPage::setUserAgent(QString());
        check(otr->httpUserAgent() == vanilla,
              "private profile restores vanilla UA");

        // Preset file: parses, has entries, carries no dead-engine
        // (MSIE/Presto/WebKit-era) or self-badged strings.
        int presetCount = 0;
        bool stalePreset = false;
        {
            QFile presets(QLatin1String(":/useragents/useragents.xml"));
            check(presets.open(QIODevice::ReadOnly),
                  "useragents.xml opens");
            QXmlStreamReader xml(&presets);
            while (!xml.atEnd()) {
                xml.readNext();
                if (!xml.isStartElement()
                    || xml.name() != QLatin1String("useragent"))
                    continue;
                ++presetCount;
                const QString preset = xml.attributes()
                    .value(QLatin1String("useragent")).toString();
                stalePreset |= preset.contains(QLatin1String("MSIE"))
                    || preset.contains(QLatin1String("Presto"))
                    || preset.contains(QLatin1String("QtWebKit"))
                    || preset.contains(QLatin1String("QtWebEngine"));
            }
            check(xml.error() == QXmlStreamReader::NoError,
                  "useragents.xml parses");
            check(presetCount >= 8, "preset count");
            check(!stalePreset, "no stale presets");
        }

        // Live check (report-only): a real Google search must not be
        // diverted to the /sorry/ interstitial.
        QObject::connect(view, &QWebEngineView::loadFinished, &application,
            [&application, view, failures](bool ok) {
            if (!ok) {
                qInfo() << "ua-smoke: live google SKIP"
                           " (load failed — offline?)";
                application.exit(failures ? 1 : 0);
                return;
            }
            const QString url = view->url().toString();
            const bool sorry = url.contains(QLatin1String("/sorry/"));
            qInfo() << "ua-smoke: live google"
                    << (sorry ? "WARN /sorry/ redirect (IP reputation,"
                               " not the UA)" : "PASS")
                    << url;
            application.exit(failures ? 1 : 0);
        });
        QTimer::singleShot(30000, &application,
            [&application, failures]() {
            qInfo() << "ua-smoke: live google SKIP (timeout — offline?)";
            application.exit(failures ? 1 : 0);
        });
        view->loadUrl(QUrl(QLatin1String(
            "https://www.google.com/search?q=arora+browser")));

        // exec() must run while 'failures' is still alive: the lambdas
        // above capture it by reference.
        return application.exec();
    }

    // Headless verification for SESS01 — the real session save/restore
    // cycle across two process runs.
    //
    // `--session-smoke` (save phase): three tabs on fixture urls with
    // the middle tab current, then a private window whose tabs must
    // never reach the session blob, then a real window close — the
    // AutoSaver in ~BrowserMainWindow is what saves the session on a
    // user-close before quitOnLastWindowClosed ends the run.  The
    // persisted blob is parsed in aboutToQuit.
    //
    // `--restore-smoke` (restore phase): startupBehavior=2 so the
    // postLaunch() queued by the BrowserApplication ctor runs
    // restoreLastSession() on the window created here; a poll then
    // verifies every tab url in order and the restored current index.
    const bool sessionSaveSmoke = args.contains(QLatin1String("--session-smoke"));
    const bool sessionRestoreSmoke = args.contains(QLatin1String("--restore-smoke"));
    if (sessionSaveSmoke || sessionRestoreSmoke) {
        int sessionFailures = 0;
        const auto sessionCheck =
            [&sessionFailures](bool ok, const char *what) {
            qInfo() << "session-smoke:" << what << (ok ? "PASS" : "FAIL");
            if (!ok)
                ++sessionFailures;
        };

        // The stub window above only hosts the WebView smokes — hide it
        // so closing the real browser window still counts as the last
        // window and ends the run.
        window.hide();
        QSettings().setValue(
            QLatin1String("tabs/confirmClosingMultipleTabs"), false);
        // A leftover crash-loop flag would open a modal prompt.
        QSettings().setValue(QLatin1String("MainWindow/restoring"), false);

        QStringList fixtureUrls;
        for (int i = 1; i <= 3; ++i) {
            const QString path = QDir::temp().filePath(
                QStringLiteral("arora-session-%1.html").arg(i));
            QFile fixture(path);
            if (fixture.open(QIODevice::WriteOnly)) {
                fixture.write(QStringLiteral(
                    "<html><head><title>arora-session-%1</title>"
                    "</head><body>%1</body></html>").arg(i).toUtf8());
            }
            fixtureUrls << QUrl::fromLocalFile(path).toString();
        }

        if (sessionSaveSmoke) {
            // postLaunch runs inside exec() — pin the startup behavior
            // to "blank" so it does not navigate the window away.
            QSettings().setValue(QLatin1String("MainWindow/startupBehavior"), 1);
            QSettings().remove(QLatin1String("sessions"));

            BrowserMainWindow *browserWindow = application.newMainWindow();
            TabWidget *tabWidget = browserWindow->tabWidget();
            tabWidget->loadUrl(QUrl(fixtureUrls.at(0)), TabWidget::CurrentTab);
            tabWidget->loadUrl(QUrl(fixtureUrls.at(1)), TabWidget::NewNotSelectedTab);
            tabWidget->loadUrl(QUrl(fixtureUrls.at(2)), TabWidget::NewNotSelectedTab);
            tabWidget->setCurrentIndex(1);

            // A private window's tabs live on the off-the-record
            // profile: serializing its tab widget must produce an
            // empty tab list, and saveSession() while private must not
            // touch the blob at all.
            BrowserApplication::setPrivate(true);
            BrowserMainWindow *privateWindow = application.newMainWindow();
            const QUrl privateUrl = QUrl::fromLocalFile(
                QDir::temp().filePath(
                    QLatin1String("arora-session-private.html")));
            privateWindow->tabWidget()->loadUrl(privateUrl,
                                                TabWidget::CurrentTab);
            {
                QByteArray privateState =
                    privateWindow->tabWidget()->saveState();
                QDataStream privateStream(privateState);
                qint32 marker = 0, version = 0;
                QStringList privateTabs;
                privateStream >> marker >> version >> privateTabs;
                sessionCheck(privateTabs.isEmpty(),
                             "private window serializes zero tabs");
            }
            application.saveSession();
            sessionCheck(
                QSettings().value(QLatin1String("sessions/lastSession"))
                    .isNull(),
                "saveSession while private writes nothing");
            privateWindow->close();
            BrowserApplication::setPrivate(false);

            QObject::connect(&application, &QCoreApplication::aboutToQuit,
                             &application,
                             [sessionCheck, fixtureUrls,
                              &sessionFailures]() {
                // Parse the blob the user's window-close just wrote:
                // magic, version, window count, then per-window states
                // carrying a tab state of url list + current index.
                const QByteArray blob =
                    QSettings()
                        .value(QLatin1String("sessions/lastSession"))
                        .toByteArray();
                QDataStream stream(blob);
                qint32 marker = 0, version = 0, windowCount = 0;
                stream >> marker >> version >> windowCount;
                sessionCheck(marker == 0xec && version == 2
                                 && windowCount == 1,
                             "session blob header");
                QStringList restoredUrls;
                qint32 restoredCurrent = -1;
                for (qint32 i = 0; i < windowCount; ++i) {
                    QByteArray windowState;
                    stream >> windowState;
                    QDataStream windowStream(windowState);
                    qint32 wmarker = 0, wversion = 0;
                    QSize size;
                    bool b1 = false, b2 = false, b3 = false;
                    QByteArray tabState;
                    windowStream >> wmarker >> wversion >> size
                        >> b1 >> b2 >> b3 >> tabState;
                    QDataStream tabStream(tabState);
                    qint32 tmarker = 0, tversion = 0;
                    tabStream >> tmarker >> tversion
                        >> restoredUrls >> restoredCurrent;
                    sessionCheck(tmarker == 0xaa && tversion == 1,
                                 "tab-state blob header");
                }
                sessionCheck(restoredUrls == fixtureUrls,
                             "session blob tab urls in order");
                sessionCheck(restoredCurrent == 1,
                             "session blob current index");
                if (sessionFailures)
                    qInfo() << "session-smoke: FAIL";
            });

            // Close the real window once the loads have settled; the
            // quit path saves the session, then lastWindowClosed quits.
            QTimer::singleShot(2500, &application, [browserWindow]() {
                browserWindow->close();
            });
            QTimer::singleShot(30000, &application, [&application]() {
                qInfo() << "session-smoke: FAIL (save phase timeout)";
                fflush(nullptr);
                std::quick_exit(1);
            });
            const int rc = application.exec();
            fflush(nullptr);
            std::quick_exit(sessionFailures ? 1 : rc);
        }

        if (sessionRestoreSmoke) {
            QSettings().setValue(QLatin1String("MainWindow/startupBehavior"), 2);
            BrowserMainWindow *browserWindow = application.newMainWindow();

            QTimer *poll = new QTimer(&application);
            auto ticks = std::make_shared<int>(0);
            QObject::connect(poll, &QTimer::timeout, &application,
                             [sessionCheck, &sessionFailures,
                              browserWindow, fixtureUrls, ticks, poll]() {
                TabWidget *tabWidget = browserWindow->tabWidget();
                QStringList got;
                bool titlesOk = true;
                for (int i = 0; i < tabWidget->count(); ++i) {
                    WebView *tab = tabWidget->webView(i);
                    if (!tab)
                        continue;
                    got << tab->url().toString();
                    // A fixture title proves the tab actually loaded,
                    // not just that its url was scheduled.
                    titlesOk &= tab->title().contains(
                        QStringLiteral("arora-session-%1").arg(i + 1));
                }
                if (got == fixtureUrls && tabWidget->currentIndex() == 1
                    && titlesOk) {
                    poll->stop();
                    sessionCheck(true, "restored tab urls in order");
                    sessionCheck(true, "restored current index");
                    sessionCheck(true, "restored tabs loaded");
                    qInfo() << "session-smoke: restore"
                            << (sessionFailures ? "FAIL" : "PASS");
                    fflush(nullptr);
                    std::quick_exit(sessionFailures ? 1 : 0);
                }
                if (++*ticks > 100) {
                    poll->stop();
                    sessionCheck(false, "restore completed");
                    qInfo() << "  got urls:" << got
                            << "index:" << tabWidget->currentIndex()
                            << "urlsMatch:" << (got == fixtureUrls)
                            << "titlesOk:" << titlesOk;
                    fflush(nullptr);
                    std::quick_exit(1);
                }
            });
            poll->start(200);
            return application.exec();
        }
    }

    // Headless measurement for PERF01 — report-only timings for the
    // three audited hot paths: cold start, tab-open latency and
    // large-history model load, plus the profile-tree permission sweep
    // (SEC12) that runs on the startup path.  Always exits 0; the
    // numbers go into task notes rather than a pass/fail gate.
    // Workload sizes are overridable through ARORA_PERF_HISTORY_N and
    // ARORA_PERF_TREE_N.
    if (args.contains(QLatin1String("--perf-smoke"))) {
        qInfo() << "perf-smoke: app-ctor" << appCtorMs << "ms"
                << "(single-instance, profile bring-up, services)";

        // First window construction ends in the first tab; extra tabs
        // measure the steady-state tab-open path.
        const qint64 windowStart = perfTimer.elapsed();
        BrowserMainWindow *perfWindow = application.newMainWindow();
        const qint64 windowMs = perfTimer.elapsed() - windowStart;
        qInfo() << "perf-smoke: first-window" << windowMs << "ms";

        const int extraTabs = 8;
        TabWidget *perfTabs = perfWindow->tabWidget();
        const qint64 tabsStart = perfTimer.elapsed();
        for (int i = 0; i < extraTabs; ++i)
            perfTabs->makeNewTab(false);
        const qint64 tabsMs = perfTimer.elapsed() - tabsStart;
        qInfo() << "perf-smoke: new-tab avg"
                << qRound(tabsMs / double(extraTabs) * 10) / 10.0
                << "ms over" << extraTabs << "tabs";

        // Large-history load: seed the on-disk format directly
        // (oldest->newest QByteArray blocks, newest-first in memory),
        // then measure a fresh manager's parse and its two lazy model
        // warmups, plus the per-visit prepend cost at scale.
        const int envHistoryN = qEnvironmentVariableIntValue("ARORA_PERF_HISTORY_N");
        const int historyN = envHistoryN > 0 ? envHistoryN : 40000;
        const QString historyPath =
            BrowserPaths::dataFilePath(QLatin1String("history"));
        {
            QFile seed(historyPath);
            if (!seed.open(QIODevice::WriteOnly)) {
                qInfo() << "perf-smoke: FAIL (cannot write history seed)";
                return 1;
            }
            QDataStream out(&seed);
            const QDateTime base =
                QDateTime::currentDateTime().addDays(-7);
            for (int i = 0; i < historyN; ++i) {
                QByteArray data;
                QDataStream stream(&data, QIODevice::WriteOnly);
                stream << quint32(HistoryParser::Version)
                       << QStringLiteral("http://example.com/%1").arg(i)
                       << base.addSecs(i)
                       << QStringLiteral("page %1").arg(i);
                out << data;
            }
        }
        QElapsedTimer step;
        step.start();
        HistoryManager bench;
        const qint64 historyCtorMs = step.elapsed();
        step.restart();
        bench.historyFilterModel()->rowCount();
        const qint64 historyFilterMs = step.elapsed();
        step.restart();
        bench.historyTreeModel()->rowCount(QModelIndex());
        const qint64 historyTreeMs = step.elapsed();
        step.restart();
        bench.addHistoryEntry(QStringLiteral("http://example.com/new"));
        const qint64 historyAddMs = step.elapsed();
        qInfo() << "perf-smoke: history" << historyN << "entries —"
                << "load" << historyCtorMs << "ms,"
                << "filter-model" << historyFilterMs << "ms,"
                << "tree-model" << historyTreeMs << "ms,"
                << "add-entry" << historyAddMs << "ms";

        // SEC12 profile-tree sweep: recursive owner-only enforcement
        // runs on the startup path (applySettings) — measure it on a
        // synthetic tree.  Files are created with the default umask so
        // the first pass repairs and the second verifies.
        const int envTreeN = qEnvironmentVariableIntValue("ARORA_PERF_TREE_N");
        const int treeN = envTreeN > 0 ? envTreeN : 5000;
        QTemporaryDir tree(QDir::temp().filePath(
            QLatin1String("arora-perf-XXXXXX")));
        if (!tree.isValid()) {
            qInfo() << "perf-smoke: FAIL (cannot create tree dir)";
            return 1;
        }
        for (int i = 0; i < treeN; ++i) {
            QDir().mkpath(tree.path() + QStringLiteral("/d%1")
                          .arg(i % 25));
            QFile file(tree.path() + QStringLiteral("/d%1/f%2")
                       .arg(i % 25).arg(i));
            if (file.open(QIODevice::WriteOnly))
                file.close();
        }
        step.restart();
        BrowserProfile::ensureUserOnlyPermissions(tree.path());
        const qint64 treeFirstMs = step.elapsed();
        step.restart();
        BrowserProfile::ensureUserOnlyPermissions(tree.path());
        const qint64 treeSecondMs = step.elapsed();
        qInfo() << "perf-smoke: perm-sweep" << treeN << "files —"
                << "first" << treeFirstMs << "ms,"
                << "second" << treeSecondMs << "ms";

        // Give the WebEngine child one event-loop spin so teardown
        // follows the normal path.
        QTimer::singleShot(0, &application,
                           [&application]() { application.exit(0); });
        return application.exec();
    }

    return application.exec();
}
