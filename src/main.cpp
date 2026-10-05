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

#include "cookiejar.h"
#include "downloadmanager.h"
#include "networkaccessmanager.h"
#include "schemeaccesshandler.h"
#include "webpage.h"
#include "webview.h"

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
    // MIG15: the default profile persists cookies/cache to disk, and a
    // CookieJar applies the accept/exception policy to its cookie store.
    QWebEngineProfile *profile = QWebEngineProfile::defaultProfile();
    CookieJar *cookieJar = new CookieJar(profile, &application);
    SchemeAccessHandler::installAll(profile, &application);

    // MIG04: application-side fetch manager (opensearch, adblock
    // subscriptions).  TODO(MIG15): BrowserApplication owns the
    // singleton.
    NetworkAccessManager *networkAccessManager = new NetworkAccessManager(&application);

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

    return application.exec();
}
