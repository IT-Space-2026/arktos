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

// UPD01: Help > Check for Updates — the GitHub releases lookup is
// exercised end-to-end against a loopback HTTP server (no real
// github.com traffic ever leaves the test), plus the version/asset
// matching statics, the tor refusal and the Help-menu wiring.

#include <QtTest/QtTest>
#include <QtGui/QtGui>
#include <qapplication.h>
#include <qcombobox.h>
#include <qlabel.h>
#include <qmenu.h>
#include <qmenubar.h>
#include <qpushbutton.h>
#include <qtcpserver.h>
#include <qtcpsocket.h>

#include "browserapplication.h"
#include "browsermainwindow.h"
#include "updatechecker.h"
#include "updatedialog.h"
#include "qtest_arora.h"
#include "qtry.h"

// Minimal canned releases endpoint: records every request head and
// answers with the configured status + JSON body.
class ReleaseServer : public QObject
{
    Q_OBJECT

public:
    explicit ReleaseServer(QObject *parent = nullptr)
        : QObject(parent)
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            QTcpSocket *socket = m_server.nextPendingConnection();
            socket->setParent(this);
            connect(socket, &QTcpSocket::readyRead, this,
                    [this, socket]() {
                if (!socket->peek(8192).contains("\r\n\r\n"))
                    return;
                lastRequest = socket->readAll();
                ++requestCount;
                QByteArray statusLine =
                    QByteArrayLiteral("HTTP/1.0 200 OK\r\n");
                if (status == 404)
                    statusLine =
                        QByteArrayLiteral("HTTP/1.0 404 Not Found\r\n");
                else if (status == 500)
                    statusLine = QByteArrayLiteral(
                        "HTTP/1.0 500 Internal Server Error\r\n");
                socket->write(statusLine
                    + QByteArrayLiteral(
                        "Content-Type: application/json\r\n"
                        "Content-Length: ")
                    + QByteArray::number(body.size())
                    + QByteArrayLiteral("\r\nConnection: close\r\n\r\n")
                    + body);
                socket->disconnectFromHost();
            });
        });
    }

    bool start()
    {
        return m_server.listen(QHostAddress::LocalHost);
    }
    QUrl url(const QString &path) const
    {
        return QUrl(QString::fromLatin1("http://127.0.0.1:%1%2")
            .arg(m_server.serverPort()).arg(path));
    }

    int status = 200;
    QByteArray body;
    int requestCount = 0;
    QByteArray lastRequest;

private:
    QTcpServer m_server;
};

static QByteArray releaseJson(const QString &tag, const QString &assets)
{
    return QByteArrayLiteral("{\"tag_name\":\"") + tag.toUtf8()
        + QByteArrayLiteral("\",\"name\":\"Arora ") + tag.toUtf8()
        + QByteArrayLiteral(
            "\",\"html_url\":\"http://127.0.0.1/releases/x\",\"assets\":[")
        + assets.toUtf8() + QByteArrayLiteral("]}");
}

static QByteArray assetJson(const QString &name, const QString &url)
{
    return QByteArrayLiteral("{\"name\":\"") + name.toUtf8()
        + QByteArrayLiteral("\",\"browser_download_url\":\"")
        + url.toUtf8() + QByteArrayLiteral("\",\"size\":100}");
}

// Per-OS package list mimicking a CI release.
static QByteArray allPlatformAssets()
{
    const QList<QByteArray> assets = {
        assetJson(QLatin1String("Arora 9.9 Installer.exe"),
                  QLatin1String("http://127.0.0.1/win-installer.exe")),
        assetJson(QLatin1String("arora-9.9-win.zip"),
                  QLatin1String("http://127.0.0.1/win.zip")),
        assetJson(QLatin1String("Arora-9.9.dmg"),
                  QLatin1String("http://127.0.0.1/mac.dmg")),
        assetJson(QLatin1String("arora_9.9_amd64.deb"),
                  QLatin1String("http://127.0.0.1/linux.deb")),
        assetJson(QLatin1String("arora-9.9.x86_64.rpm"),
                  QLatin1String("http://127.0.0.1/linux.rpm")),
        assetJson(QLatin1String("arora-9.9.flatpak"),
                  QLatin1String("http://127.0.0.1/linux.flatpak")),
    };
    QByteArray joined;
    for (const QByteArray &asset : assets) {
        if (!joined.isEmpty())
            joined += ',';
        joined += asset;
    }
    return joined;
}

class tst_UpdateChecker : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void versionCompare();
    void assetMatching();
    void upToDate();
    void updateAvailable();
    void noReleases();
    void malformedJson();
    void oversizedBody();
    void httpError();
    void untrustedAssetUrls();
    void torRefusesCheck();
    void torDialogSinks();
    void helpMenuEntry();
    void dialogLegs();
};

void tst_UpdateChecker::initTestCase()
{
    qRegisterMetaType<UpdateChecker::Result>();
}

void tst_UpdateChecker::cleanupTestCase()
{
    BrowserApplication::setTorMode(false);
}

void tst_UpdateChecker::versionCompare()
{
    // Normalization: v-prefix, prerelease/build suffixes and the app's
    // own git decoration all drop before comparing.
    QCOMPARE(UpdateChecker::normalizedVersion(QLatin1String("v1.2.3")),
             QLatin1String("1.2.3"));
    QCOMPARE(UpdateChecker::normalizedVersion(
                 QLatin1String("1.2.3-rc1+build5")),
             QLatin1String("1.2.3"));
    QCOMPARE(UpdateChecker::normalizedVersion(
                 QLatin1String("0.12 (Git: abc123 4)")),
             QLatin1String("0.12"));
    QCOMPARE(UpdateChecker::normalizedVersion(QLatin1String("  0.9  ")),
             QLatin1String("0.9"));

    QCOMPARE(UpdateChecker::compareVersions(
                 QLatin1String("1.2.3"), QLatin1String("1.2.3")), 0);
    QCOMPARE(UpdateChecker::compareVersions(
                 QLatin1String("v0.13"), QLatin1String("0.12")), 1);
    QCOMPARE(UpdateChecker::compareVersions(
                 QLatin1String("0.2"), QLatin1String("0.12")), -1);
    QCOMPARE(UpdateChecker::compareVersions(
                 QLatin1String("1.0"), QLatin1String("1.0.0")), 0);
    QCOMPARE(UpdateChecker::compareVersions(
                 QLatin1String("2.0-rc1"), QLatin1String("1.9")), 1);
    // A non-semver tag falls back to string compare — equality still
    // answers "same release" honestly.
    QCOMPARE(UpdateChecker::compareVersions(
                 QLatin1String("nightly"), QLatin1String("nightly")), 0);
    QVERIFY(UpdateChecker::compareVersions(
                QLatin1String("beta"), QLatin1String("alpha")) > 0);
}

void tst_UpdateChecker::assetMatching()
{
    // Build the asset list once through the real fetch+parse path.
    const QByteArray payload = releaseJson(QLatin1String("v9.9"),
                                           QString::fromUtf8(allPlatformAssets()));
    QList<UpdateChecker::ReleaseAsset> assets;
    {
        ReleaseServer server;
        QVERIFY(server.start());
        server.body = payload;
        UpdateChecker checker;
        checker.setApiUrl(server.url(QLatin1String("/releases/latest")));
        QSignalSpy spy(&checker, &UpdateChecker::finished);
        checker.check();
        QVERIFY(spy.wait(10000) || !spy.isEmpty());
        QCOMPARE(spy.count(), 1);
        const UpdateChecker::Result done =
            qvariant_cast<UpdateChecker::Result>(spy.first().first());
        assets = done.assets;
        QCOMPARE(done.status, UpdateChecker::Status::UpdateAvailable);
    }
    QCOMPARE(assets.size(), 6);

    // Windows: the installer exe wins; the zip is never a candidate.
    const UpdateChecker::PlatformPick win =
        UpdateChecker::platformAssetsFor(
            QLatin1String("windows"), QString(), assets);
    QVERIFY(!win.candidates.isEmpty());
    QCOMPARE(win.candidates.first().name,
             QLatin1String("Arora 9.9 Installer.exe"));
    for (const UpdateChecker::ReleaseAsset &asset : win.candidates)
        QVERIFY(asset.name.endsWith(QLatin1String(".exe")));
    QVERIFY(!win.ambiguous);

    // macOS: dmg only.
    const UpdateChecker::PlatformPick mac =
        UpdateChecker::platformAssetsFor(
            QLatin1String("macos"), QString(), assets);
    QCOMPARE(mac.candidates.size(), 1);
    QCOMPARE(mac.candidates.first().name, QLatin1String("Arora-9.9.dmg"));

    // Debian-family → deb first, flatpak fallback; rpm-family → rpm.
    const UpdateChecker::PlatformPick deb =
        UpdateChecker::platformAssetsFor(
            QLatin1String("linux"), QLatin1String("debian"), assets);
    QCOMPARE(deb.candidates.first().name,
             QLatin1String("arora_9.9_amd64.deb"));
    QVERIFY(!deb.ambiguous);
    const UpdateChecker::PlatformPick ubuntu =
        UpdateChecker::platformAssetsFor(
            QLatin1String("linux"),
            QLatin1String("ubuntu debian"), assets);
    QCOMPARE(ubuntu.candidates.first().name,
             QLatin1String("arora_9.9_amd64.deb"));
    const UpdateChecker::PlatformPick rpm =
        UpdateChecker::platformAssetsFor(
            QLatin1String("linux"),
            QLatin1String("fedora"), assets);
    QCOMPARE(rpm.candidates.first().name,
             QLatin1String("arora-9.9.x86_64.rpm"));

    // Unknown distro: every plausible candidate, flatpak leading —
    // ambiguous so the dialog must let the user pick.
    const UpdateChecker::PlatformPick unknown =
        UpdateChecker::platformAssetsFor(
            QLatin1String("linux"), QLatin1String("plan9"), assets);
    QVERIFY(unknown.ambiguous);
    QCOMPARE(unknown.candidates.size(), 3);
    QCOMPARE(unknown.candidates.first().name,
             QLatin1String("arora-9.9.flatpak"));

    // Release with no host package → empty candidates (dialog falls
    // back to the release page instead of a foreign download).
    const UpdateChecker::PlatformPick none =
        UpdateChecker::platformAssetsFor(
            QLatin1String("linux"), QLatin1String("debian"),
            QList<UpdateChecker::ReleaseAsset>());
    QVERIFY(none.candidates.isEmpty());
}

void tst_UpdateChecker::upToDate()
{
    ReleaseServer server;
    QVERIFY(server.start());
    const QString current = UpdateChecker::normalizedVersion(
        QCoreApplication::applicationVersion());
    server.body = releaseJson(current, QString());

    UpdateChecker checker;
    checker.setApiUrl(server.url(QLatin1String("/releases/latest")));
    QSignalSpy spy(&checker, &UpdateChecker::finished);
    checker.check();
    QVERIFY(spy.wait(10000) || !spy.isEmpty());
    QCOMPARE(spy.count(), 1);
    const UpdateChecker::Result result =
        qvariant_cast<UpdateChecker::Result>(spy.first().first());
    QCOMPARE(result.status, UpdateChecker::Status::UpToDate);
    QCOMPARE(server.requestCount, 1);
    // The request carried the app User-Agent the GitHub API demands.
    QVERIFY(server.lastRequest.contains("User-Agent: Arora/"));
}

void tst_UpdateChecker::updateAvailable()
{
    ReleaseServer server;
    QVERIFY(server.start());
    server.body = releaseJson(QLatin1String("v999.0.0"),
                              QString::fromUtf8(allPlatformAssets()));

    UpdateChecker checker;
    checker.setApiUrl(server.url(QLatin1String("/releases/latest")));
    QSignalSpy spy(&checker, &UpdateChecker::finished);
    checker.check();
    QVERIFY(spy.wait(10000) || !spy.isEmpty());
    QCOMPARE(spy.count(), 1);
    const UpdateChecker::Result result =
        qvariant_cast<UpdateChecker::Result>(spy.first().first());
    QCOMPARE(result.status, UpdateChecker::Status::UpdateAvailable);
    QCOMPARE(result.latestVersion, QLatin1String("v999.0.0"));
    QCOMPARE(result.assets.size(), 6);
    QCOMPARE(result.releasePageUrl,
             QUrl(QLatin1String("http://127.0.0.1/releases/x")));
}

void tst_UpdateChecker::noReleases()
{
    ReleaseServer server;
    QVERIFY(server.start());
    server.status = 404;
    server.body = QByteArrayLiteral("{\"message\":\"Not Found\"}");

    UpdateChecker checker;
    checker.setApiUrl(server.url(QLatin1String("/releases/latest")));
    QSignalSpy spy(&checker, &UpdateChecker::finished);
    checker.check();
    QVERIFY(spy.wait(10000) || !spy.isEmpty());
    QCOMPARE(spy.count(), 1);
    const UpdateChecker::Result result =
        qvariant_cast<UpdateChecker::Result>(spy.first().first());
    QCOMPARE(result.status, UpdateChecker::Status::NoReleases);
}

void tst_UpdateChecker::malformedJson()
{
    ReleaseServer server;
    QVERIFY(server.start());
    server.body = QByteArrayLiteral("this is not json {");

    UpdateChecker checker;
    checker.setApiUrl(server.url(QLatin1String("/releases/latest")));
    QSignalSpy spy(&checker, &UpdateChecker::finished);
    checker.check();
    QVERIFY(spy.wait(10000) || !spy.isEmpty());
    QCOMPARE(spy.count(), 1);
    const UpdateChecker::Result result =
        qvariant_cast<UpdateChecker::Result>(spy.first().first());
    QCOMPARE(result.status, UpdateChecker::Status::Error);
    QVERIFY(!result.errorString.isEmpty());
}

void tst_UpdateChecker::oversizedBody()
{
    // AUDIT03: a manifest far beyond the parse bound is refused fast —
    // the reply must never reach the JSON parser.
    ReleaseServer server;
    QVERIFY(server.start());
    server.body = QByteArray(1200 * 1024, '{');

    UpdateChecker checker;
    checker.setApiUrl(server.url(QLatin1String("/releases/latest")));
    QSignalSpy spy(&checker, &UpdateChecker::finished);
    checker.check();
    QVERIFY(spy.wait(10000) || !spy.isEmpty());
    QCOMPARE(spy.count(), 1);
    const UpdateChecker::Result result =
        qvariant_cast<UpdateChecker::Result>(spy.first().first());
    QCOMPARE(result.status, UpdateChecker::Status::Error);
    QVERIFY(result.errorString.contains(QLatin1String("oversized")));
}

void tst_UpdateChecker::httpError()
{
    ReleaseServer server;
    QVERIFY(server.start());
    server.status = 500;
    server.body = QByteArrayLiteral("{}");

    UpdateChecker checker;
    checker.setApiUrl(server.url(QLatin1String("/releases/latest")));
    QSignalSpy spy(&checker, &UpdateChecker::finished);
    checker.check();
    QVERIFY(spy.wait(10000) || !spy.isEmpty());
    QCOMPARE(spy.count(), 1);
    const UpdateChecker::Result result =
        qvariant_cast<UpdateChecker::Result>(spy.first().first());
    QCOMPARE(result.status, UpdateChecker::Status::Error);
}

void tst_UpdateChecker::untrustedAssetUrls()
{
    // AUDIT04: server JSON must never steer the download target off
    // the release surface — javascript:/file:/data:/off-host URLs are
    // dropped at parse, and html_url is scheme-bound too.
    const QUrl api(QStringLiteral("https://api.github.com/x"));
    QVERIFY(!UpdateChecker::isTrustedDownloadUrl(
        QUrl(QLatin1String("javascript:alert(1)")), api));
    QVERIFY(!UpdateChecker::isTrustedDownloadUrl(
        QUrl(QLatin1String("file:///etc/passwd")), api));
    QVERIFY(!UpdateChecker::isTrustedDownloadUrl(
        QUrl(QLatin1String("data:application/x-deb;base64,AA==")), api));
    QVERIFY(!UpdateChecker::isTrustedDownloadUrl(
        QUrl(QLatin1String("http://evil.example/arora.deb")), api));
    QVERIFY(!UpdateChecker::isTrustedDownloadUrl(
        QUrl(QLatin1String("https://evil.example/arora.deb")), api));
    // A github host over cleartext is a downgrade, not an asset.
    QVERIFY(!UpdateChecker::isTrustedDownloadUrl(
        QUrl(QLatin1String("http://github.com/a/x.deb")), api));
    // Credential-bearing URLs are refused even on a trusted host.
    QVERIFY(!UpdateChecker::isTrustedDownloadUrl(
        QUrl(QLatin1String("https://user:pw@github.com/a/x.deb")), api));
    QVERIFY(UpdateChecker::isTrustedDownloadUrl(
        QUrl(QLatin1String(
            "https://github.com/o/r/releases/download/v9.9/a.deb")),
        api));
    QVERIFY(UpdateChecker::isTrustedDownloadUrl(
        QUrl(QLatin1String(
            "https://objects.githubusercontent.com/x/y")), api));
    // Self-hosted api (loopback tests, GHE): the api's own host is
    // trusted, other hosts and scheme downgrades are not.
    const QUrl loopback(QStringLiteral("http://127.0.0.1:8/r"));
    QVERIFY(UpdateChecker::isTrustedDownloadUrl(
        QUrl(QLatin1String("http://127.0.0.1/asset.deb")), loopback));
    QVERIFY(!UpdateChecker::isTrustedDownloadUrl(
        QUrl(QLatin1String("http://127.0.0.2/asset.deb")), loopback));
    const QUrl ghe(QStringLiteral("https://ghe.corp.example/api"));
    QVERIFY(!UpdateChecker::isTrustedDownloadUrl(
        QUrl(QLatin1String("http://ghe.corp.example/a.deb")), ghe));

    // End-to-end: a hostile payload still reports the update, but the
    // booby-trapped assets never reach the candidate list and the
    // injected page url is never stored.
    ReleaseServer server;
    QVERIFY(server.start());
    const QList<QByteArray> assets = {
        assetJson(QLatin1String("evil-js.deb"),
                  QLatin1String("javascript:alert(1)")),
        assetJson(QLatin1String("evil-file.deb"),
                  QLatin1String("file:///etc/passwd")),
        assetJson(QLatin1String("evil-host.deb"),
                  QLatin1String("https://evil.example/arora.deb")),
        assetJson(QLatin1String("arora_9.9_amd64.deb"),
                  server.url(QLatin1String("/a.deb")).toString()),
    };
    QByteArray joined;
    for (const QByteArray &asset : assets) {
        if (!joined.isEmpty())
            joined += ',';
        joined += asset;
    }
    QByteArray payload = releaseJson(QLatin1String("v999.0.0"),
                                     QString::fromUtf8(joined));
    payload.replace("http://127.0.0.1/releases/x",
                    "javascript:alert(1)");
    server.body = payload;

    UpdateChecker checker;
    checker.setApiUrl(server.url(QLatin1String("/releases/latest")));
    QSignalSpy spy(&checker, &UpdateChecker::finished);
    checker.check();
    QVERIFY(spy.wait(10000) || !spy.isEmpty());
    QCOMPARE(spy.count(), 1);
    const UpdateChecker::Result result =
        qvariant_cast<UpdateChecker::Result>(spy.first().first());
    QCOMPARE(result.status, UpdateChecker::Status::UpdateAvailable);
    QCOMPARE(result.assets.size(), 1);
    QCOMPARE(result.assets.first().name,
             QLatin1String("arora_9.9_amd64.deb"));
    QVERIFY(!result.releasePageUrl.isValid()
            || result.releasePageUrl.scheme()
                != QLatin1String("javascript"));
}

void tst_UpdateChecker::torRefusesCheck()
{
    ReleaseServer server;
    QVERIFY(server.start());
    server.body = releaseJson(QLatin1String("v999.0.0"), QString());

    BrowserApplication::setTorMode(true);
    UpdateChecker checker;
    checker.setApiUrl(server.url(QLatin1String("/releases/latest")));
    QSignalSpy spy(&checker, &UpdateChecker::finished);
    checker.check();
    // The refusal is synchronous — no request may leave the process.
    QVERIFY(spy.wait(5000) || !spy.isEmpty());
    QCOMPARE(spy.count(), 1);
    const UpdateChecker::Result result =
        qvariant_cast<UpdateChecker::Result>(spy.first().first());
    QCOMPARE(result.status, UpdateChecker::Status::Error);
    QCOMPARE(server.requestCount, 0);
    BrowserApplication::setTorMode(false);
}

void tst_UpdateChecker::torDialogSinks()
{
    // AUDIT04: refusing check() alone is not enough — the dialog's
    // sinks (download button, releases link) must refuse on their own
    // so a github.com url can never reach clearnet from a tor
    // process.
    BrowserApplication::setTorMode(true);
    ReleaseServer server;
    QVERIFY(server.start());
    server.body = releaseJson(QLatin1String("v999.0.0"),
                              QString::fromUtf8(allPlatformAssets()));
    {
        UpdateDialog dialog;
        dialog.checker()->setApiUrl(
            server.url(QLatin1String("/releases/latest")));
        QSignalSpy spy(dialog.checker(), &UpdateChecker::finished);
        dialog.check();
        QVERIFY(spy.wait(5000) || !spy.isEmpty());
        QCOMPARE(dialog.statusText(),
                 QLatin1String("Couldn't check for updates."));
        QVERIFY(!dialog.findChild<QPushButton *>(
                     QLatin1String("updateDownloadButton"))
                     ->isEnabled());
        QCOMPARE(server.requestCount, 0);

        // The link and download slots refuse too — both report the
        // tor refusal instead of touching the network/handler.
        QVERIFY(QMetaObject::invokeMethod(&dialog, "viewAllReleases"));
        QVERIFY(dialog.statusText().contains(QLatin1String("Tor")));
        QVERIFY(QMetaObject::invokeMethod(&dialog, "downloadLatest"));
        QVERIFY(dialog.statusText().contains(QLatin1String("Tor")));
    }
    BrowserApplication::setTorMode(false);
}

void tst_UpdateChecker::helpMenuEntry()
{
    BrowserMainWindow *window = new BrowserMainWindow;
    QMenu *help = nullptr;
    for (QAction *menuAction : window->menuBar()->actions()) {
        if (menuAction->menu()
                && menuAction->menu()->title().remove(QLatin1Char('&'))
                    == QLatin1String("Help"))
            help = menuAction->menu();
    }
    QVERIFY(help);
    QAction *check = nullptr;
    int position = -1;
    for (int i = 0; i < help->actions().size(); ++i) {
        QAction *action = help->actions().at(i);
        if (action->text() == QLatin1String("Check for Updates...")) {
            check = action;
            position = i;
        }
    }
    QVERIFY(check);
    QVERIFY(check->isEnabled());
    // It sits in the about group — after the separator that follows
    // the language entry, before About Qt.
    QVERIFY(position > 0);
    QVERIFY(help->actions().at(position - 1)->isSeparator()
        || help->actions().at(position - 1)->text()
            .contains(QLatin1String("language")));
    bool aboutAfter = false;
    for (int i = position + 1; i < help->actions().size(); ++i) {
        if (help->actions().at(i)->text().startsWith(QLatin1String("About")))
            aboutAfter = true;
    }
    QVERIFY(aboutAfter);
    window->close();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    // A tor window keeps the entry visible but disabled with the
    // honest reason in its tooltip.
    BrowserApplication::setTorMode(true);
    BrowserMainWindow *torWindow = new BrowserMainWindow;
    QMenu *torHelp = nullptr;
    QList<QAction *> menuActions = torWindow->menuBar()->actions();
    if (menuActions.isEmpty() && torWindow->appMenu())
        menuActions = torWindow->appMenu()->actions();
    for (QAction *menuAction : menuActions) {
        QMenu *menu = menuAction->menu();
        if (menu && menu->title().remove(QLatin1Char('&'))
                == QLatin1String("Help"))
            torHelp = menu;
    }
    QVERIFY(torHelp);
    QAction *torCheck = nullptr;
    for (QAction *action : torHelp->actions()) {
        if (action->text() == QLatin1String("Check for Updates..."))
            torCheck = action;
    }
    QVERIFY(torCheck);
    QVERIFY(!torCheck->isEnabled());
    QVERIFY(torCheck->toolTip().contains(QLatin1String("Tor")));
    torWindow->close();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    BrowserApplication::setTorMode(false);
}

void tst_UpdateChecker::dialogLegs()
{
    ReleaseServer server;
    QVERIFY(server.start());

    // Up to date.
    const QString current = UpdateChecker::normalizedVersion(
        QCoreApplication::applicationVersion());
    server.body = releaseJson(current, QString());
    {
        UpdateDialog dialog;
        dialog.checker()->setApiUrl(
            server.url(QLatin1String("/releases/latest")));
        QSignalSpy spy(dialog.checker(), &UpdateChecker::finished);
        dialog.check();
        QVERIFY(spy.wait(10000) || !spy.isEmpty());
        QCOMPARE(dialog.statusText(), QLatin1String("You're up to date."));
        QVERIFY(!dialog.findChild<QPushButton *>(
                     QLatin1String("updateDownloadButton"))->isEnabled());
    }

    // Newer version with packages — download armed.
    server.body = releaseJson(QLatin1String("v999.0.0"),
                              QString::fromUtf8(allPlatformAssets()));
    {
        UpdateDialog dialog;
        dialog.checker()->setApiUrl(
            server.url(QLatin1String("/releases/latest")));
        QSignalSpy spy(dialog.checker(), &UpdateChecker::finished);
        dialog.check();
        QVERIFY(spy.wait(10000) || !spy.isEmpty());
        QCOMPARE(dialog.statusText(),
                 QLatin1String("Version v999.0.0 is available."));
        QPushButton *download = dialog.findChild<QPushButton *>(
            QLatin1String("updateDownloadButton"));
        QVERIFY(download->isEnabled());
        QCOMPARE(download->text(),
                 QLatin1String("Download Latest Version"));
    }

    // Newer version but no package for this host → the button
    // degrades to the release page instead of a wrong download.
    server.body = releaseJson(QLatin1String("v999.0.0"),
        QString::fromUtf8(assetJson(
            QLatin1String("Arora 9.9 Installer.exe"),
            QLatin1String("http://127.0.0.1/win.exe"))));
    {
        UpdateDialog dialog;
        dialog.checker()->setApiUrl(
            server.url(QLatin1String("/releases/latest")));
        QSignalSpy spy(dialog.checker(), &UpdateChecker::finished);
        dialog.check();
        QVERIFY(spy.wait(10000) || !spy.isEmpty());
        QCOMPARE(dialog.statusText(),
                 QLatin1String("Version v999.0.0 is available."));
        QPushButton *download = dialog.findChild<QPushButton *>(
            QLatin1String("updateDownloadButton"));
        QVERIFY(download->isEnabled());
#if defined(Q_OS_WIN)
        // On Windows the installer IS the right package.
        QCOMPARE(download->text(),
                 QLatin1String("Download Latest Version"));
#else
        QCOMPARE(download->text(), QLatin1String("Open Release Page"));
#endif
    }

    // No releases published — honest empty state, not up-to-date.
    server.status = 404;
    {
        UpdateDialog dialog;
        dialog.checker()->setApiUrl(
            server.url(QLatin1String("/releases/latest")));
        QSignalSpy spy(dialog.checker(), &UpdateChecker::finished);
        dialog.check();
        QVERIFY(spy.wait(10000) || !spy.isEmpty());
        QCOMPARE(dialog.statusText(),
                 QLatin1String("No releases published yet."));
    }

    // Server reachable but payload garbage → couldn't check.
    server.status = 200;
    server.body = QByteArrayLiteral("<html>nope</html>");
    {
        UpdateDialog dialog;
        dialog.checker()->setApiUrl(
            server.url(QLatin1String("/releases/latest")));
        QSignalSpy spy(dialog.checker(), &UpdateChecker::finished);
        dialog.check();
        QVERIFY(spy.wait(10000) || !spy.isEmpty());
        QCOMPARE(dialog.statusText(),
                 QLatin1String("Couldn't check for updates."));
        QVERIFY(!dialog.findChild<QLabel *>(
                     QLatin1String("updateDetailLabel"))->isHidden());
    }
}

QTEST_MAIN(tst_UpdateChecker)
#include "tst_updatechecker.moc"
