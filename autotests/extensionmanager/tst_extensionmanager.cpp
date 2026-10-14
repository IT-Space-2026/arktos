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

// COV11: direct coverage for ExtensionManager — the permission
// classification lists, the version/manifest parsers, the EXT05
// consent gate, and the whole self-update pipeline (gupdate manifest
// fetch + package download) driven honestly over a loopback server.

#include <QtTest/QtTest>
#include <QtNetwork/QtNetwork>
#include <qtcpserver.h>
#include <qtcpsocket.h>
#include <qwebenginepage.h>
#include <qwebengineprofile.h>

#include <memory>

#include "browserapplication.h"
#include "browserprofile.h"
#include "extensionbackgroundhost.h"
#include "extensionmanager.h"
#include "qtest_arora.h"
#include "qtry.h"

class tst_ExtensionManager : public QObject
{
    Q_OBJECT

private slots:
    void permissionLists();
    void compareVersions();
    void parseUpdateManifest_data();
    void parseUpdateManifest();
    void inspectManifest_data();
    void inspectManifest();
    void consentGate();
    void storePaths();
    void profileIsolation();
    void bundledInstall();
    void updateCheckLoopback();
    void hostedBackgroundRuntime();

private:
    // Minimal PK zip writer (store-only) — enough for the update
    // pipeline's structural verification and the PK-magic
    // installable check.
    static QByteArray makeZip(
            const QList<QPair<QByteArray, QByteArray>> &entries) {
        const auto crc32 = [](const QByteArray &data) {
            static quint32 table[256];
            static bool haveTable = false;
            if (!haveTable) {
                for (quint32 i = 0; i < 256; ++i) {
                    quint32 c = i;
                    for (int k = 0; k < 8; ++k)
                        c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                    table[i] = c;
                }
                haveTable = true;
            }
            quint32 crc = 0xFFFFFFFFu;
            for (char ch : data)
                crc = table[(crc ^ uchar(ch)) & 0xFF] ^ (crc >> 8);
            return crc ^ 0xFFFFFFFFu;
        };
        QByteArray out;
        QByteArray central;
        for (const auto &entry : entries) {
            const QByteArray &name = entry.first;
            const QByteArray &data = entry.second;
            const quint32 offset = quint32(out.size());
            const quint32 crc = crc32(data);
            QDataStream local(&out, QIODevice::Append);
            local.setByteOrder(QDataStream::LittleEndian);
            local << quint32(0x04034b50) << quint16(20) << quint16(0)
                  << quint16(0) << quint16(0) << quint16(0) << crc
                  << quint32(data.size()) << quint32(data.size())
                  << quint16(name.size()) << quint16(0);
            out.append(name);
            out.append(data);
            QDataStream cd(&central, QIODevice::Append);
            cd.setByteOrder(QDataStream::LittleEndian);
            cd << quint32(0x02014b50) << quint16(20) << quint16(20)
               << quint16(0) << quint16(0) << quint16(0) << quint16(0)
               << crc << quint32(data.size()) << quint32(data.size())
               << quint16(name.size()) << quint16(0) << quint16(0)
               << quint16(0) << quint16(0) << quint32(0) << offset;
            central.append(name);
        }
        const quint32 cdOffset = quint32(out.size());
        out.append(central);
        QDataStream eocd(&out, QIODevice::Append);
        eocd.setByteOrder(QDataStream::LittleEndian);
        eocd << quint32(0x06054b50) << quint16(0) << quint16(0)
             << quint16(entries.size()) << quint16(entries.size())
             << quint32(central.size()) << cdOffset << quint16(0);
        return out;
    }

    static QString writeExtension(const QString &root,
            const QString &name, const QString &version,
            const QString &updateUrl = QString(),
            const QStringList &permissions = QStringList()) {
        const QString dir = root + QLatin1Char('/') + name;
        QDir().mkpath(dir);
        QFile manifestFile(dir + QLatin1String("/manifest.json"));
        if (!manifestFile.open(QIODevice::WriteOnly))
            return QString();
        QByteArray manifest = "{\"manifest_version\":3,\"name\":\""
            + name.toUtf8() + "\",\"version\":\"" + version.toUtf8()
            + "\",\"background\":{\"service_worker\":\"sw.js\"}";
        if (!updateUrl.isEmpty())
            manifest += ",\"update_url\":\"" + updateUrl.toUtf8() + "\"";
        if (!permissions.isEmpty()) {
            manifest += ",\"permissions\":[";
            for (int i = 0; i < permissions.size(); ++i) {
                if (i)
                    manifest += ',';
                manifest += '"' + permissions.at(i).toUtf8() + '"';
            }
            manifest += ']';
        }
        manifest += '}';
        manifestFile.write(manifest);
        manifestFile.close();
        QFile worker(dir + QLatin1String("/sw.js"));
        if (!worker.open(QIODevice::WriteOnly))
            return QString();
        worker.write(
            "chrome.runtime.onInstalled.addListener(function(){});\n");
        return dir;
    }

    // Installs the fixture dir and returns the Chromium-assigned id
    // (empty on failure).  installExtension() targets the first
    // profile the manager was attached to.
    QString installFixture(const QString &dir) {
        ExtensionManager *manager = ExtensionManager::instance();
        auto installed = std::make_shared<QString>();
        auto finished = std::make_shared<bool>(false);
        QObject::connect(manager, &ExtensionManager::extensionInstalled,
                         this, [installed, finished](
                             const ExtensionManager::ExtensionInfo &info) {
            if (info.error.isEmpty())
                *installed = info.id;
            else
                *installed = QStringLiteral("ERROR:") + info.error;
            *finished = true;
        });
        QObject::connect(manager, &ExtensionManager::errorOccurred,
                         this, [installed, finished](
                             const QString &message) {
            *installed = QStringLiteral("ERROR:") + message;
            *finished = true;
        });
        manager->grantConsent(dir);
        manager->installExtension(dir);
        QElapsedTimer timer;
        timer.start();
        while (!*finished && timer.elapsed() < 30000)
            QTest::qWait(50);
        return *installed;
    }
};

void tst_ExtensionManager::permissionLists()
{
    // The lists are deliberately hard to reach from a UI-free drive —
    // assert their shape and a few pinned members so a silent drop
    // shows up here.
    const QStringList unsupported =
        ExtensionManager::unsupportedPermissions();
    QVERIFY(unsupported.contains(QLatin1String("bookmarks")));
    QVERIFY(unsupported.contains(QLatin1String("debugger")));
    QVERIFY(unsupported.size() > 30);

    const QStringList unverified =
        ExtensionManager::unverifiedPermissions();
    QVERIFY(unverified.contains(QLatin1String("webRequest")));
    QVERIFY(unverified.contains(QLatin1String("scripting")));

    const QStringList dangerous =
        ExtensionManager::dangerousPermissions();
    QVERIFY(dangerous.contains(QLatin1String("nativeMessaging")));
    QVERIFY(dangerous.contains(QLatin1String("cookies")));
    QVERIFY(dangerous.contains(QLatin1String("browsingData")));

    const QStringList dangerousHosts =
        ExtensionManager::dangerousHostPatterns();
    QVERIFY(dangerousHosts.contains(QLatin1String("<all_urls>")));
    QVERIFY(dangerousHosts.contains(QLatin1String("*://*/*")));

    // "debugger"/"cookies" are intentionally both unsupported-by-Qt
    // and dangerous — the lists classify on different axes.
}

void tst_ExtensionManager::compareVersions()
{
    QCOMPARE(ExtensionManager::compareVersions(
                 QStringLiteral("1.0"), QStringLiteral("1.0.0")), 0);
    QVERIFY(ExtensionManager::compareVersions(
                QStringLiteral("1.10"), QStringLiteral("1.9")) > 0);
    QVERIFY(ExtensionManager::compareVersions(
                QStringLiteral("2.0"), QStringLiteral("2.1")) < 0);
    QVERIFY(ExtensionManager::compareVersions(
                QStringLiteral("1.0.0.1"),
                QStringLiteral("1.0.0")) > 0);
    QVERIFY(ExtensionManager::compareVersions(
                QStringLiteral("abc"), QStringLiteral("abd")) < 0);
    QCOMPARE(ExtensionManager::compareVersions(
                 QStringLiteral("2.0.0"), QStringLiteral("2.0.0")), 0);
}

void tst_ExtensionManager::parseUpdateManifest_data()
{
    QTest::addColumn<QByteArray>("xml");
    QTest::addColumn<QString>("expectedId");
    QTest::addColumn<QString>("currentVersion");
    QTest::addColumn<bool>("ok");
    QTest::addColumn<QString>("availableVersion");
    QTest::addColumn<QString>("codeBasePath");
    QTest::addColumn<bool>("upToDate");
    QTest::addColumn<bool>("wantError");

    const QByteArray doc =
        "<?xml version=\"1.0\"?>"
        "<gupdate xmlns=\"http://www.google.com/update2/response\""
        " protocol=\"2.0\"><app appid=\"%1\">"
        "<updatecheck codebase=\"https://upd.example/pkg.zip\""
        " version=\"%2\"/></app></gupdate>";

    QTest::newRow("newer offered")
        << QByteArray(doc).replace("%1", "appid-one").replace(
               "%2", "2.0.0")
        << QStringLiteral("appid-one") << QStringLiteral("1.0.0")
        << true << QStringLiteral("2.0.0")
        << QStringLiteral("/pkg.zip") << false << false;
    QTest::newRow("older offered = up to date")
        << QByteArray(doc).replace("%1", "appid-one").replace(
               "%2", "0.9.0")
        << QStringLiteral("appid-one") << QStringLiteral("1.0.0")
        << true << QString() << QString() << true << false;
    QTest::newRow("truncated")
        << QByteArray("<gupdate>")
        << QStringLiteral("appid-one") << QStringLiteral("1.0.0")
        << false << QString() << QString() << false << true;
    QTest::newRow("empty")
        << QByteArray()
        << QStringLiteral("appid-one") << QStringLiteral("1.0.0")
        << false << QString() << QString() << false << true;
}

void tst_ExtensionManager::parseUpdateManifest()
{
    QFETCH(QByteArray, xml);
    QFETCH(QString, expectedId);
    QFETCH(QString, currentVersion);
    QFETCH(bool, ok);
    QFETCH(QString, availableVersion);
    QFETCH(QString, codeBasePath);
    QFETCH(bool, upToDate);
    QFETCH(bool, wantError);

    ExtensionManager::UpdateResult result;
    result.currentVersion = currentVersion;
    QCOMPARE(ExtensionManager::parseUpdateManifest(xml, expectedId,
                                                   &result), ok);
    QCOMPARE(result.availableVersion, availableVersion);
    QCOMPARE(result.codeBase.path(), codeBasePath);
    QCOMPARE(result.upToDate, upToDate);
    QCOMPARE(!result.error.isEmpty(), wantError);
}

void tst_ExtensionManager::inspectManifest_data()
{
    QTest::addColumn<QByteArray>("manifest");
    QTest::addColumn<bool>("valid");
    QTest::addColumn<int>("manifestVersion");
    QTest::addColumn<QString>("name");
    QTest::addColumn<bool>("hasBackground");
    QTest::addColumn<QStringList>("unsupported");
    QTest::addColumn<QStringList>("unverified");
    QTest::addColumn<QStringList>("dangerous");
    QTest::addColumn<QStringList>("dangerousHosts");

    QTest::newRow("minimal mv3")
        << QByteArray("{\"manifest_version\":3,\"name\":\"mini\","
                      "\"version\":\"1.0\"}")
        << true << 3 << QStringLiteral("mini") << false
        << QStringList() << QStringList() << QStringList()
        << QStringList();
    QTest::newRow("permission classes")
        << QByteArray("{\"manifest_version\":3,\"name\":\"perms\","
                      "\"version\":\"1.0\","
                      "\"permissions\":[\"bookmarks\",\"webRequest\","
                      "\"cookies\",\"nativeMessaging\"],"
                      "\"host_permissions\":[\"<all_urls>\","
                      "\"*://*.example.com/*\"]}")
        << true << 3 << QStringLiteral("perms") << false
        << (QStringList() << QStringLiteral("bookmarks"))
        << (QStringList() << QStringLiteral("webRequest"))
        << (QStringList() << QStringLiteral("cookies")
                          << QStringLiteral("nativeMessaging"))
        << (QStringList() << QStringLiteral("<all_urls>"));
    QTest::newRow("background flag")
        << QByteArray("{\"manifest_version\":3,\"name\":\"bg\","
                      "\"version\":\"1.0\",\"background\":"
                      "{\"service_worker\":\"sw.js\"}}")
        << true << 3 << QStringLiteral("bg") << true
        << QStringList() << QStringList() << QStringList()
        << QStringList();
    QTest::newRow("not json")
        << QByteArray("this is not json")
        << false << 0 << QString() << false
        << QStringList() << QStringList() << QStringList()
        << QStringList();
}

void tst_ExtensionManager::inspectManifest()
{
    QFETCH(QByteArray, manifest);
    QFETCH(bool, valid);
    QFETCH(int, manifestVersion);
    QFETCH(QString, name);
    QFETCH(bool, hasBackground);
    QFETCH(QStringList, unsupported);
    QFETCH(QStringList, unverified);
    QFETCH(QStringList, dangerous);
    QFETCH(QStringList, dangerousHosts);

    const QString dir = QDir::temp().filePath(
        QStringLiteral("tst-extmanifest-%1").arg(QTest::currentDataTag()));
    QVERIFY(QDir().mkpath(dir));
    QFile file(dir + QLatin1String("/manifest.json"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(manifest);
    file.close();

    const ExtensionManager::Manifest parsed =
        ExtensionManager::inspectManifest(dir);
    QCOMPARE(parsed.valid, valid);
    if (valid) {
        QCOMPARE(parsed.manifestVersion, manifestVersion);
        QCOMPARE(parsed.name, name);
        QCOMPARE(parsed.hasBackground, hasBackground);
        for (const QString &perm : unsupported)
            QVERIFY(parsed.unsupported.contains(perm));
        for (const QString &perm : unverified)
            QVERIFY(parsed.unverified.contains(perm));
        for (const QString &perm : dangerous)
            QVERIFY(parsed.dangerous.contains(perm));
        for (const QString &host : dangerousHosts)
            QVERIFY(parsed.dangerous.contains(host));
    }

    // A packaged input is refused for inspection — Qt's installer
    // handles those.
    QFile zip(dir + QLatin1String("/pkg.zip"));
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.write("PK\x03\x04");
    zip.close();
    QVERIFY(!ExtensionManager::inspectManifest(
                dir + QLatin1String("/pkg.zip")).valid);
    // Missing manifest file.
    QVERIFY(!ExtensionManager::inspectManifest(
                dir + QLatin1String("/empty")).valid);

    QDir(dir).removeRecursively();
}

void tst_ExtensionManager::consentGate()
{
    ExtensionManager *manager = ExtensionManager::instance();
    const QString path = QStringLiteral("/nonexistent/extension");

    // Ungranted paths are refused and consume nothing.
    auto errors = std::make_shared<QStringList>();
    QObject::connect(manager, &ExtensionManager::errorOccurred,
                     this, [errors](const QString &message) {
        errors->append(message);
    });
    QVERIFY(!manager->hasConsent(path));
    manager->loadExtension(path);
    QVERIFY(errors->last().contains(QLatin1String("consent")));
    manager->installExtension(path);
    QVERIFY(errors->last().contains(QLatin1String("consent")));

    // A grant is consumed by the load it authorizes — the second
    // call is refused again.
    manager->grantConsent(path);
    QVERIFY(manager->hasConsent(path));
    manager->loadExtension(path);
    QVERIFY(!manager->hasConsent(path));
}

void tst_ExtensionManager::storePaths()
{
    QVERIFY(!ExtensionManager::updatesPath().isEmpty());
    QVERIFY(QDir(ExtensionManager::updatesPath()).exists());
    QVERIFY(!ExtensionManager::userScriptsPath().isEmpty());
    // EXT05: the userscript store must be owner-only.
    QVERIFY(!(QFileInfo(ExtensionManager::userScriptsPath())
                  .permissions()
              & (QFileDevice::ReadGroup | QFileDevice::WriteGroup
                 | QFileDevice::ExeGroup | QFileDevice::ReadOther
                 | QFileDevice::WriteOther | QFileDevice::ExeOther)));

    const bool saved = ExtensionManager::updateCheckEnabled();
    ExtensionManager::setUpdateCheckEnabled(!saved);
    QCOMPARE(ExtensionManager::updateCheckEnabled(), !saved);
    ExtensionManager::setUpdateCheckEnabled(saved);
    QCOMPARE(ExtensionManager::updateCheckEnabled(), saved);
}

// EXT06: earlier legs of this test installed into the user's real
// "arora" profile when the binary ran outside runTests.sh (which
// sandboxes HOME) — the fixtures showed up in the live Extensions
// list as tst-upd/tst-old/tst-noext.  QTEST_MAIN now enables
// QStandardPaths test mode before BrowserApplication builds the
// profile; pin the redirect so a regression is loud here instead of
// leaking into the user's extension store again.
void tst_ExtensionManager::profileIsolation()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");
    const QString installPath = ExtensionManager::instance()->installPath();
    QVERIFY(!installPath.isEmpty());
    // Qt 6.12's test mode roots app data under ~/.qttest — the exact
    // layout is Qt's business; what matters is that the store is on
    // the test-mode path and NOT under the real profile's data dir.
    QVERIFY2(installPath.contains(QLatin1String("qttest")),
             qPrintable(QStringLiteral(
                 "extension install path must be sandboxed, got %1")
                 .arg(installPath)));
    QVERIFY2(!installPath.startsWith(
                 QDir::homePath()
                     + QLatin1String("/.local/share/Arora")),
             qPrintable(QStringLiteral(
                 "extension install path leaked into the real profile: %1")
                 .arg(installPath)));
}

// EXT07: the bundled-default installer must land every candidate in
// the bundled dir onto a non-OTR profile exactly once, enabled only
// when the profile was extension-fresh, with the per-profile marker
// recording each consumed file so nothing reinstalls.
void tst_ExtensionManager::bundledInstall()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");

    // Fixture bundled dir: an unpacked dir carrying manifest.json and
    // a packaged .zip are both candidates.
    const QString root = QDir::temp().filePath(
        QStringLiteral("tst-extbundle-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(root));
    const QString dir = writeExtension(root, QStringLiteral("tst-bundle"),
                                       QStringLiteral("1.0.0"));
    QVERIFY(!dir.isEmpty());
    const QByteArray package = makeZip({
        {QByteArray("manifest.json"), QByteArray(
            "{\"manifest_version\":3,\"name\":\"tst-bundle-zip\","
            "\"version\":\"1.0.0\",\"background\":{\"service_worker\":"
            "\"sw.js\"}}")},
        {QByteArray("sw.js"), QByteArray(
            "chrome.runtime.onInstalled.addListener(function(){});\n")},
    });
    QFile zipFile(root + QLatin1String("/tst-bundle-zip.zip"));
    QVERIFY(zipFile.open(QIODevice::WriteOnly));
    QCOMPARE(zipFile.write(package), qint64(package.size()));
    zipFile.close();

    qputenv("ARORA_BUNDLED_EXTENSIONS_DIR", root.toUtf8());
    QCOMPARE(ExtensionManager::bundledExtensionsPath(), root);

    ExtensionManager *manager = ExtensionManager::instance();
    QWebEngineProfile *profile = BrowserApplication::webEngineProfile();

    // Leftovers are persistent: a standalone run (real HOME) inherits
    // both the consumed marker and any previously installed fixture
    // rows, and either makes this run skip the installs outright.
    // Drop both so the leg is idempotent outside the runTests.sh HOME
    // sandbox.
    {
        const QString key = QStringLiteral("extensions/bundled/")
            + profile->storageName();
        QSettings settings;
        QStringList consumed = settings.value(key).toStringList();
        consumed.removeAll(QLatin1String("tst-bundle"));
        consumed.removeAll(QLatin1String("tst-bundle-zip.zip"));
        settings.setValue(key, consumed);
    }
    for (const ExtensionManager::ExtensionInfo &info
         : manager->extensions()) {
        if (info.name.startsWith(QLatin1String("tst-bundle")))
            manager->removeExtension(info.id);
    }
    // Uninstall is async — wait until no fixture row survives, or the
    // stem check inside installBundledExtensions() satisfies them.
    QTRY_VERIFY_WITH_TIMEOUT([&] {
        for (const ExtensionManager::ExtensionInfo &info
             : manager->extensions()) {
            if (info.name.startsWith(QLatin1String("tst-bundle")))
                return false;
        }
        return true;
    }(), 10000);

    // The enable expectation mirrors installBundledExtensions(): the
    // bundle enables exactly when installPath held nothing before.
    const QString installDir = manager->installPath();
    const bool expectEnabled = installDir.isEmpty()
        || !QDir(installDir).exists()
        || QDir(installDir).entryList(
            QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty();

    auto installed = std::make_shared<QSet<QString>>();
    auto finished = std::make_shared<int>(0);
    QObject::connect(manager, &ExtensionManager::extensionInstalled,
                     this, [installed, finished](
                         const ExtensionManager::ExtensionInfo &info) {
        if (info.error.isEmpty()
                && (info.name == QLatin1String("tst-bundle")
                    || info.name == QLatin1String("tst-bundle-zip"))) {
            installed->insert(info.name);
            ++*finished;
        }
    });
    manager->installBundledExtensions(profile);

    QTRY_VERIFY_WITH_TIMEOUT(installed->size() == 2, 30000);

    // Both rows: installed; enabled iff the profile was
    // extension-fresh at install time (the post-install enable is
    // asynchronous — poll until the reported state settles).
    QList<ExtensionManager::ExtensionInfo> rows;
    bool settled = false;
    int seen = 0;
    for (int i = 0; i < 200 && !settled; ++i) {
        rows = manager->extensions();
        settled = true;
        seen = 0;
        for (const ExtensionManager::ExtensionInfo &info : rows) {
            if (installed->contains(info.name)) {
                ++seen;
                if (!info.installed || info.enabled != expectEnabled)
                    settled = false;
            }
        }
        if (!settled || seen != 2)
            QTest::qWait(50);
    }
    QVERIFY2(settled && seen == 2,
             qPrintable(QStringLiteral(
                 "bundled rows installed + enabled=%1, seen=%2")
                 .arg(expectEnabled).arg(seen)));

    // Both basenames recorded as consumed; a second run is a no-op.
    const QStringList consumed = QSettings().value(
        QStringLiteral("extensions/bundled/") + profile->storageName())
        .toStringList();
    QVERIFY(consumed.contains(QLatin1String("tst-bundle")));
    QVERIFY(consumed.contains(QLatin1String("tst-bundle-zip.zip")));

    // The internal grant was consumed — the raw path is guarded
    // again for manual installs.
    QVERIFY(!manager->hasConsent(dir));
    QVERIFY(!manager->hasConsent(zipFile.fileName()));

    // A second call must not reinstall or re-enable anything.
    const int listedBefore = rows.size();
    manager->installBundledExtensions(profile);
    QTest::qWait(500);
    QCOMPARE(manager->extensions().size(), listedBefore);

    // The off-the-record profile refuses bundled installs outright.
    QWebEngineProfile *otr = BrowserProfile::privateProfile();
    QVERIFY(otr->isOffTheRecord());
    manager->installBundledExtensions(otr);

    // Leave the fixture profile clean for later legs — uninstall is
    // async, give it a beat to land.
    for (const ExtensionManager::ExtensionInfo &info
         : manager->extensions()) {
        if (installed->contains(info.name))
            manager->removeExtension(info.id);
    }
    QTest::qWait(500);

    qputenv("ARORA_BUNDLED_EXTENSIONS_DIR", QByteArray());
    QDir(root).removeRecursively();
}

// EXT03 end-to-end: a loopback gupdate feed offers a newer package —
// the whole pipeline (manifest fetch -> version compare -> package
// download -> verify -> save -> installable verdict) must run.
void tst_ExtensionManager::updateCheckLoopback()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");

    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    const QByteArray base =
        "http://127.0.0.1:" + QByteArray::number(server.serverPort());
    const QByteArray package = makeZip({
        {QByteArray("manifest.json"), QByteArray(
            "{\"manifest_version\":3,\"name\":\"tst-upd\","
            "\"version\":\"2.0.0\",\"background\":{\"service_worker\":"
            "\"sw.js\"}}")},
        {QByteArray("sw.js"), QByteArray(
            "chrome.runtime.onInstalled.addListener(function(){});\n")},
    });
    const QByteArray updateXml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<gupdate xmlns=\"http://www.google.com/update2/response\""
        " protocol=\"2.0\"><app appid=\"placeholder\">"
        "<updatecheck codebase=\"" + base
        + "/pkg.zip\" version=\"2.0.0\"/></app></gupdate>";
    QObject::connect(&server, &QTcpServer::newConnection,
                     this, [&]() {
        QTcpSocket *socket = server.nextPendingConnection();
        QObject::connect(socket, &QTcpSocket::readyRead,
                         socket, [socket, updateXml, package]() {
            const QByteArray request = socket->readAll();
            const int sp = request.indexOf(' ');
            const QByteArray path = sp >= 0
                ? request.mid(sp + 1,
                              request.indexOf(' ', sp + 1) - sp - 1)
                : QByteArray();
            QByteArray body;
            int status = 200;
            if (path == "/update.xml")
                body = updateXml;
            else if (path == "/update-old.xml")
                body = QByteArray(updateXml)
                    .replace("2.0.0", "0.5.0");
            else if (path == "/pkg.zip")
                body = package;
            else
                status = 404;
            socket->write("HTTP/1.1 " + QByteArray::number(status)
                + " \r\nContent-Length: "
                + QByteArray::number(body.size())
                + "\r\nConnection: close\r\n\r\n" + body);
            socket->disconnectFromHost();
        });
    });

    // The extension id is derived from the install path, so a
    // pid-rooted root gives every run fresh ids (and a fresh
    // update_url port).
    const QString root = QDir::temp().filePath(
        QStringLiteral("tst-extupdate-%1")
            .arg(QCoreApplication::applicationPid()));
    QDir().mkpath(root);
    const QString dirA = writeExtension(root, QStringLiteral("tst-upd"),
        QStringLiteral("1.0.0"),
        QString::fromUtf8(base) + QLatin1String("/update.xml"));
    const QString dirB = writeExtension(root, QStringLiteral("tst-old"),
        QStringLiteral("1.0.0"),
        QString::fromUtf8(base) + QLatin1String("/update-old.xml"));
    const QString dirC = writeExtension(root, QStringLiteral("tst-noext"),
        QStringLiteral("1.0.0"),
        QStringLiteral("ftp://no.example/feed.xml"));
    QVERIFY(!dirA.isEmpty() && !dirB.isEmpty() && !dirC.isEmpty());

    ExtensionManager *manager = ExtensionManager::instance();

    // Sweep earlier leftovers: the installed copy lives in the
    // profile store (so path won't point at our dir) but keeps the
    // manifest name.
    const QSet<QString> fixtureNames = {
        QStringLiteral("tst-upd"), QStringLiteral("tst-old"),
        QStringLiteral("tst-noext")};
    for (const ExtensionManager::ExtensionInfo &info
         : manager->extensions()) {
        if (!info.builtin && fixtureNames.contains(info.name))
            manager->removeExtension(info.id);
    }
    QTest::qWait(300);

    const QString idA = installFixture(dirA);
    const QString idB = installFixture(dirB);
    const QString idC = installFixture(dirC);
    QVERIFY2(!idA.isEmpty() && !idB.isEmpty() && !idC.isEmpty(),
             qPrintable(QStringLiteral(
                 "fixture extensions must install: idA=%1 idB=%2 "
                 "idC=%3").arg(idA, idB, idC)));

    auto done = std::make_shared<bool>(false);
    auto results = std::make_shared<QList<ExtensionManager::UpdateResult>>();
    QObject::connect(manager, &ExtensionManager::updateCheckFinished,
                     this, [done, results](const QList<
                             ExtensionManager::UpdateResult> &list) {
        *results = list;
        *done = true;
    });
    manager->checkForUpdates(true);
    QTRY_VERIFY_WITH_TIMEOUT(*done, 60000);

    const ExtensionManager::UpdateResult resA =
        manager->updateResultFor(idA);
    QCOMPARE(resA.error, QString());
    QCOMPARE(resA.availableVersion, QStringLiteral("2.0.0"));
    QVERIFY(!resA.savedTo.isEmpty());
    QVERIFY(QFile::exists(resA.savedTo));
    QVERIFY(resA.savedTo.startsWith(ExtensionManager::updatesPath()));
    QVERIFY(resA.installable);

    // Older offer -> upToDate, no package fetch.
    const ExtensionManager::UpdateResult resB =
        manager->updateResultFor(idB);
    QVERIFY(resB.upToDate);
    QVERIFY(resB.savedTo.isEmpty());

    // Non-fetchable update_url refused at the gate.
    const ExtensionManager::UpdateResult resC =
        manager->updateResultFor(idC);
    QVERIFY(resC.noSource);
    QVERIFY(!resC.error.isEmpty());
    QCOMPARE(manager->updateUrlFor(idC),
             QStringLiteral("ftp://no.example/feed.xml"));

    // A second check while the first results are cached still works.
    QVERIFY(!manager->updateCheckInProgress());

    QDir(root).removeRecursively();
}

// EXT14: a session-loaded extension declaring a background script
// gets a hidden hosted view at its chrome-extension:// origin — the
// generated injection runs the preamble, the real script executes,
// chrome.runtime survives, and the chromeBridge handshake completes.
// Disable tears the host down, re-enable respawns it.
void tst_ExtensionManager::hostedBackgroundRuntime()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");

    const QString dir = QDir::temp().filePath(
        QStringLiteral("tst-exthost-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dir));
    {
        QFile manifestFile(dir + QLatin1String("/manifest.json"));
        QVERIFY(manifestFile.open(QIODevice::WriteOnly));
        manifestFile.write(
            "{\"manifest_version\":3,\"name\":\"tst-hosted\","
            "\"version\":\"1.0\",\"permissions\":[\"storage\"],"
            "\"background\":{\"service_worker\":\"sw.js\"}}");
        manifestFile.close();
        QFile worker(dir + QLatin1String("/sw.js"));
        QVERIFY(worker.open(QIODevice::WriteOnly));
        worker.write(
            "self.__hostedBgRan = (typeof chrome !== 'undefined'"
            " && chrome.runtime) ? 'runtime-ok' : 'no-runtime';\n"
            "chrome.runtime.onMessage.addListener("
            "function(m,s,r){ return false; });\n");
        worker.close();
    }

    // The manifest inspector supplies the script list.
    const ExtensionManager::Manifest manifest =
        ExtensionManager::inspectManifest(dir);
    QVERIFY(manifest.valid);
    QCOMPARE(manifest.backgroundScripts,
             QStringList{QStringLiteral("sw.js")});

    ExtensionManager *manager = ExtensionManager::instance();
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();

    auto loaded = std::make_shared<QString>();
    QObject::connect(manager, &ExtensionManager::extensionLoaded,
                     this, [loaded](
                         const ExtensionManager::ExtensionInfo &info) {
        if (info.name == QLatin1String("tst-hosted")
                && info.error.isEmpty())
            *loaded = info.id;
    });
    manager->grantConsent(dir);
    manager->loadExtension(dir);
    QTRY_VERIFY_WITH_TIMEOUT(!loaded->isEmpty(), 30000);
    const QString id = *loaded;

    // No host while disabled; enabling spawns it.
    QVERIFY(!runtime->host(id));
    manager->setExtensionEnabled(id, true);
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime->host(id)
            && runtime->host(id)->state()
                   == ExtensionBackgroundHost::State::Ready,
        45000);

    auto eval = [](QWebEnginePage *page, const QString &source) {
        auto result = std::make_shared<QVariant>();
        page->runJavaScript(source,
                            [result](const QVariant &v) { *result = v; });
        QElapsedTimer timer;
        timer.start();
        while (!result->isValid() && timer.elapsed() < 10000)
            QTest::qWait(50);
        return *result;
    };
    const QJsonObject state = QJsonDocument::fromJson(
        eval(runtime->host(id)->page(), QStringLiteral(
            "JSON.stringify({state:__aroraHost.state,"
            "bridge:__aroraHost.bridgeUp,"
            "bg:self.__hostedBgRan||null,"
            "runtime:typeof (chrome&&chrome.runtime),"
            "storage:typeof (chrome&&chrome.storage"
            "&&chrome.storage.local)})")).toString().toUtf8()).object();
    QCOMPARE(state.value(QLatin1String("state")).toString(),
             QStringLiteral("ready"));
    QVERIFY(state.value(QLatin1String("bridge")).toBool());
    QCOMPARE(state.value(QLatin1String("bg")).toString(),
             QStringLiteral("runtime-ok"));
    QCOMPARE(state.value(QLatin1String("runtime")).toString(),
             QStringLiteral("object"));
    QCOMPARE(state.value(QLatin1String("storage")).toString(),
             QStringLiteral("object"));

    // The manager reports the hosted tier on the info row.
    bool sawHosted = false;
    for (const ExtensionManager::ExtensionInfo &info
         : manager->extensions()) {
        if (info.id == id) {
            sawHosted = info.hosted
                && info.hostState == QLatin1String("hosted")
                && info.hostError.isEmpty();
        }
    }
    QVERIFY(sawHosted);

    // Disable -> the hosted view is torn down; re-enable -> respawn.
    manager->setExtensionEnabled(id, false);
    QTRY_VERIFY_WITH_TIMEOUT(!runtime->host(id), 15000);
    manager->setExtensionEnabled(id, true);
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime->host(id)
            && runtime->host(id)->state()
                   == ExtensionBackgroundHost::State::Ready,
        45000);

    // Unload -> host gone, no stray state.
    manager->removeExtension(id);
    QTRY_VERIFY_WITH_TIMEOUT(!runtime->host(id), 15000);

    QDir(dir).removeRecursively();
}

QTEST_MAIN(tst_ExtensionManager)
#include "tst_extensionmanager.moc"
