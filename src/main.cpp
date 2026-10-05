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
#include "webview.h"

#include <QtCore/QDebug>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
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

    QApplication::setApplicationName(QStringLiteral("arora"));
    QApplication::setOrganizationName(QStringLiteral("Arora"));

    QApplication application(argc, argv);

    // MIG03: app-wide profile wiring. BrowserApplication will own this in
    // MIG15: the default profile persists cookies/cache to disk, and a
    // CookieJar applies the accept/exception policy to its cookie store.
    QWebEngineProfile *profile = QWebEngineProfile::defaultProfile();
    CookieJar *cookieJar = new CookieJar(profile, &application);

    QMainWindow window;
    window.setWindowTitle(QStringLiteral("Arora"));

    WebView *view = new WebView(profile, &window);
    window.setCentralWidget(view);

    const QStringList args = application.arguments();
    const QString firstUrl = (args.count() > 1 && !args.at(1).startsWith(QLatin1Char('-')))
            ? args.at(1) : QStringLiteral("about:blank");
    view->loadUrl(QUrl(firstUrl));

    // Headless verification hook: exit once the first page load
    // finishes so CI can prove WebEngine ran (autotests/smoke style).
    if (args.contains(QLatin1String("--quit-after-load")))
        QObject::connect(view, &QWebEngineView::loadFinished,
                         &application, &QApplication::quit);

    window.show();

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
