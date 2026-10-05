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
#include "bookmarknode.h"
#include "bookmarksmanager.h"
#include "bookmarksmodel.h"
#include "cookiejar.h"
#include "downloadmanager.h"
#include "historymanager.h"
#include "locationbar.h"
#include "networkaccessmanager.h"
#include "opensearchengine.h"
#include "opensearchmanager.h"
#include "opensearchreader.h"
#include "opensearchwriter.h"
#include "schemeaccesshandler.h"
#include "toolbarsearch.h"
#include "webpage.h"
#include "webview.h"
#include "xbelreader.h"
#include "xbelwriter.h"

#include <QtCore/QBuffer>
#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtWebEngineCore/QWebEngineDownloadRequest>
#include <QtWebEngineCore/QWebEngineLoadingInfo>
#include <QtWebEngineCore/QWebEngineProfile>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMainWindow>

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

// TODO(MIG15): replace this skeleton with BrowserApplication —
// single-instance via QLocalServer/QLocalSocket, session restore,
// QCommandLineParser, translator loading, WebEngine init order.
// TODO(MIG14): replace the stub window below with BrowserMainWindow.
int main(int argc, char **argv)
{
    Q_INIT_RESOURCE(htmls);
    Q_INIT_RESOURCE(data);

    // Custom schemes (arora-file://) must be declared before QApplication.
    SchemeAccessHandler::registerUrlSchemes();
    // abp:subscribe?... links for AdBlock subscriptions (MIG09).
    AdBlockSchemeAccessHandler::registerUrlScheme();
    // arora-resource:// serves the bundled $redirect= adblock stubs.
    AdBlockResourceHandler::registerUrlScheme();

    QApplication::setApplicationName(QStringLiteral("arora"));
    QApplication::setOrganizationName(QStringLiteral("Arora"));

    QApplication application(argc, argv);

    const QStringList args = application.arguments();
    // --adblock-smoke / --adblock-list-smoke add custom rules and
    // download lists, which are persisted to the app data dir; isolate
    // the writes so the test leaves no residue.
    if (args.contains(QLatin1String("--adblock-smoke"))
        || args.contains(QLatin1String("--adblock-list-smoke"))
        || args.contains(QLatin1String("--adblock-rust-smoke")))
        QStandardPaths::setTestModeEnabled(true);

    // MIG03: app-wide profile wiring. BrowserApplication will own this in
    // MIG15.  The normal browsing profile must be a NAMED profile:
    // QWebEngineProfile::defaultProfile() is off-the-record (nothing —
    // cookies, cache, storage — persists to disk).  A named profile
    // gives Arora's normal browsing its persistent state; private
    // browsing gets the lazily-created off-the-record profile.
    QWebEngineProfile *profile =
        new QWebEngineProfile(QStringLiteral("arora"), &application);
    CookieJar *cookieJar = new CookieJar(profile, &application);
    SchemeAccessHandler::installAll(profile, &application);

    // MIG04: application-side fetch manager (opensearch, adblock
    // subscriptions).  TODO(MIG15): BrowserApplication delegates to
    // the singleton.
    NetworkAccessManager *networkAccessManager = NetworkAccessManager::instance();

    // MIG05: intercepts every profile it is installed on and turns
    // downloadRequested into DownloadItem rows.  TODO(MIG15): also call
    // installOnProfile() on the off-the-record private profile.
    DownloadManager *downloadManager = new DownloadManager();
    downloadManager->installOnProfile(profile);

    // MIG09: profile-level url request interceptor replaces the
    // WebKit-era QNetworkAccessManager hook for ad blocking; also
    // installs the abp: subscription scheme handler.  TODO(MIG15):
    // install on the off-the-record private profile too.
    AdBlockManager::instance()->installOnProfile(profile);

    QMainWindow window;
    window.setWindowTitle(QStringLiteral("Arora"));

    WebView *view = new WebView(profile, &window);
    window.setCentralWidget(view);

    const QString firstUrl = (args.count() > 1 && !args.at(1).startsWith(QLatin1Char('-')))
            ? args.at(1) : QStringLiteral("about:blank");
    view->loadUrl(QUrl(firstUrl));

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
        QTimer::singleShot(3000, &application, [&application, cookieJar]() {
            const QList<QNetworkCookie> allowed =
                cookieJar->cookiesForUrl(QUrl(QLatin1String("http://example.com/")));
            const QList<QNetworkCookie> blocked =
                cookieJar->cookiesForUrl(QUrl(QLatin1String("http://blocked.example/")));
            bool ok = blocked.isEmpty();
            bool found = false;
            foreach (const QNetworkCookie &cookie, allowed)
                found |= (cookie.name() == "arora_smoke");
            ok = ok && found;
            qInfo() << "cookie-smoke:" << (ok ? "PASS" : "FAIL")
                    << "(allowed:" << allowed.count() << "blocked:" << blocked.count() << ")";
            // leave no test residue in the saved exception list
            cookieJar->setBlockedCookies(QStringList());
            application.exit(ok ? 0 : 1);
        });
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

            // &nbsp; expansion (the pre-Qt6 entity resolver's job)
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
                       == QString::fromUtf8("a\xc2\xa0" "b");
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

        int stage = 0;
        QObject::connect(view->webPage(), &QWebEnginePage::loadingChanged,
                         &application,
                         [view, &application, &stage](const QWebEngineLoadingInfo &info) {
            if (info.status() != QWebEngineLoadingInfo::LoadFailedStatus)
                return;
            const QString host = info.url().host();
            const bool blockedByInterceptor =
                info.errorString().contains(QLatin1String("ERR_BLOCKED_BY_CLIENT"))
                || info.errorString().contains(QLatin1String("ERR_ACCESS_DENIED"));
            if (stage == 0 && host == QLatin1String("adblock-smoke.invalid")) {
                qInfo() << "adblock-smoke: blocked navigation"
                        << (blockedByInterceptor ? "PASS" : "FAIL") << info.errorString();
                if (!blockedByInterceptor) {
                    application.exit(1);
                    return;
                }
                stage = 1;
                view->loadUrl(QUrl(QLatin1String("http://allowed-smoke.invalid/")));
            } else if (stage == 1 && host == QLatin1String("allowed-smoke.invalid")) {
                const bool pass = !blockedByInterceptor;
                qInfo() << "adblock-smoke: exception navigation"
                        << (pass ? "PASS" : "FAIL") << info.errorString();
                if (!pass) {
                    application.exit(1);
                    return;
                }
                stage = 2;
                view->loadUrl(QUrl(QLatin1String("about:blank")));
            }
        });
        QObject::connect(view, &QWebEngineView::loadFinished, &application,
                         [view, &application, &stage](bool ok) {
            if (stage != 2 || !ok
                || view->url() != QUrl(QLatin1String("about:blank")))
                return;
            stage = 3;
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

    return application.exec();
}
