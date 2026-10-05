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
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtWebEngineCore/QWebEngineDownloadRequest>
#include <QtWebEngineCore/QWebEngineProfile>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMainWindow>

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

    QApplication::setApplicationName(QStringLiteral("arora"));
    QApplication::setOrganizationName(QStringLiteral("Arora"));

    QApplication application(argc, argv);

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

    QMainWindow window;
    window.setWindowTitle(QStringLiteral("Arora"));

    WebView *view = new WebView(profile, &window);
    window.setCentralWidget(view);

    const QStringList args = application.arguments();
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

    return application.exec();
}
