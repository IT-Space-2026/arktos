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

#include <QtCore/QUrl>
#include <QtWebEngineWidgets/QWebEngineView>
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

    QMainWindow window;
    window.setWindowTitle(QStringLiteral("Arora"));

    QWebEngineView *view = new QWebEngineView(&window);
    window.setCentralWidget(view);
    view->load(QUrl(QStringLiteral("about:blank")));

    // Headless verification hook: exit once the first page load
    // finishes so CI can prove WebEngine ran (autotests/smoke style).
    if (application.arguments().contains(QLatin1String("--quit-after-load")))
        QObject::connect(view, &QWebEngineView::loadFinished,
                         &application, &QApplication::quit);

    window.show();
    return application.exec();
}
