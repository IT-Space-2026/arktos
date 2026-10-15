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
#include "browsermainwindow.h"
#include "browserpaths.h"
#include "browserprofile.h"
#include "extensionbackgroundhost.h"
#include "extensioneventhub.h"
#include "extensionmanager.h"
#include "extensionshimbackend.h"
#include "engineinterface.h"
#include "qtest_arora.h"
#include "qtry.h"
#include "tabwidget.h"
#include "webview.h"

#include <qframe.h>
#include <qmenu.h>
#include <qsignalspy.h>
#include <qwebengineview.h>

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
    void extensionShimBackend();
    void extensionMessageRelay();
    void extensionEventSurfaces();
    void extensionPopupContext();

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

    // EXT15: drive the chrome.* gapfill shim end to end inside the
    // hosted page — bridge-backed storage round trip + onChanged,
    // a real host-timer alarm firing onAlarm, action setters landing
    // in the C++ state registry, permissions/getURL answered from the
    // handshake manifest, sendMessage resolving through the relay
    // path, and a typed 'unsupported' error surfacing as a gap.
    runtime->host(id)->page()->runJavaScript(QStringLiteral(
        "self.__probe = {done:false};"
        "(async function(){"
        " var out={};"
        " var area=__aroraShim.storageArea('session');"
        " await area.set({k:'v'});"
        " var got=await area.get('k');"
        " out.stored=(got&&got.k==='v');"
        " var fired=false;"
        " area.onChanged.addListener(function(ch,a){"
        "   if(a==='session'&&ch.k2&&ch.k2.newValue===42)fired=true;});"
        " await area.set({k2:42});"
        " out.onChanged=fired;"
        " var listedP=chrome.alarms.create('t',"
        "   {delayInMinutes:0.002})"
        "   .then(function(){return chrome.alarms.get('t');})"
        "   .then(function(v){out.alarmListed=!!(v&&v.name==='t');});"
        " await new Promise(function(r){"
        "   var done=false;"
        "   chrome.alarms.onAlarm.addListener(function(a){"
        "     if(!done&&a.name==='t'){done=true;"
        "       out.alarmFired=true;r();}});"
        "   setTimeout(function(){if(!done){done=true;r();}},20000);"
        " });"
        " await listedP;"
        " await chrome.alarms.clearAll();"
        " await chrome.action.setTitle({title:'Shim Title'});"
        " await chrome.action.setBadgeText({text:'7'});"
        " out.title=await chrome.action.getTitle({});"
        " out.badge=await chrome.action.getBadgeText({});"
        " out.url=chrome.runtime.getURL('x.html')"
        "   ===('chrome-extension://'+chrome.runtime.id+'/x.html');"
        " out.perm=await chrome.permissions.contains("
        "   {permissions:['storage']});"
        " var caught='';"
        " try{await chrome.notifications.create("
        "   {type:'basic',title:'x',message:'y'});"
        "   caught='resolved';"
        " }catch(e){caught=e.code||String(e);}"
        " out.notifError=caught;"
        " var msgReplied=false;"
        " await new Promise(function(r){"
        "   try{chrome.runtime.sendMessage({probe:1},function(){"
        "         msgReplied=true;r();});"
        "   }catch(e){r();}"
        "   setTimeout(r,8000);});"
        " out.msgCallback=msgReplied;"
        " __probe.out=out;__probe.done=true;"
        "})().catch(function(e){"
        "  __probe.err=String(e&&e.stack||e);__probe.done=true;});"),
        [](const QVariant &) {});
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(runtime->host(id)->page(),
             QStringLiteral("self.__probe&&self.__probe.done"))
            .toBool(), 30000);
    const QJsonObject probe = QJsonDocument::fromJson(
        eval(runtime->host(id)->page(), QStringLiteral(
            "JSON.stringify({out:self.__probe.out||{},"
            "err:self.__probe.err||null,gaps:__aroraShim.gaps})"))
            .toString().toUtf8()).object();
    QVERIFY2(probe.value(QLatin1String("err")).toString().isEmpty(),
             qPrintable(QStringLiteral("probe threw: %1")
                 .arg(probe.value(QLatin1String("err")).toString())));
    const QJsonObject out = probe.value(QLatin1String("out")).toObject();
    QVERIFY2(out.value(QLatin1String("stored")).toBool(),
             "bridge storage set/get round trip");
    QVERIFY2(out.value(QLatin1String("onChanged")).toBool(),
             "storage onChanged delivered");
    QVERIFY2(out.value(QLatin1String("alarmListed")).toBool(),
             "alarms.get returns the created alarm");
    QVERIFY2(out.value(QLatin1String("alarmFired")).toBool(),
             "host timer fired alarms.onAlarm");
    QCOMPARE(out.value(QLatin1String("title")).toString(),
             QStringLiteral("Shim Title"));
    QCOMPARE(out.value(QLatin1String("badge")).toString(),
             QStringLiteral("7"));
    QVERIFY(out.value(QLatin1String("url")).toBool());
    QVERIFY(out.value(QLatin1String("perm")).toBool());
    QCOMPARE(out.value(QLatin1String("notifError")).toString(),
             QStringLiteral("unsupported"));
    QVERIFY(out.value(QLatin1String("msgCallback")).toBool());

    // Action state flows into the C++ registry EXT08's button strip
    // reads; the typed notifications error reached the host's gap
    // list instead of silently no-oping.
    const ExtensionShimBackend::ActionState action =
        ExtensionShimBackend::actionState(id);
    QCOMPARE(action.title, QStringLiteral("Shim Title"));
    QCOMPARE(action.badgeText, QStringLiteral("7"));
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime->host(id)->shimGaps().contains(
            QStringLiteral("chrome.notifications.create: unsupported")),
        10000);

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

// EXT15: the host-side backend behind ChromeBridge::invoke() —
// per-extension storage namespaces (local persisted, session
// in-memory), QTimer-backed alarms, the action-state registry, i18n
// locale files, manifest-derived permissions, and the diagnostics
// channel.  Every call answers {ok,result} or {ok:false,error} —
// no silent no-ops.
void tst_ExtensionManager::extensionShimBackend()
{
    const QString dir = QDir::temp().filePath(
        QStringLiteral("tst-extshim-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dir + QLatin1String("/_locales/en")));
    {
        QFile manifestFile(dir + QLatin1String("/manifest.json"));
        QVERIFY(manifestFile.open(QIODevice::WriteOnly));
        manifestFile.write(
            "{\"manifest_version\":3,\"name\":\"tst-shim\","
            "\"version\":\"2.0\",\"default_locale\":\"en\","
            "\"permissions\":[\"storage\",\"alarms\"],"
            "\"host_permissions\":[\"https://*.example.com/*\"]}");
        manifestFile.close();
        QFile msgs(dir
            + QLatin1String("/_locales/en/messages.json"));
        QVERIFY(msgs.open(QIODevice::WriteOnly));
        msgs.write("{\"hello\":{\"message\":\"Hello $1\"},"
                   "\"bye\":{\"message\":\"Bye\"}}");
        msgs.close();
    }
    const QString id = QStringLiteral("tstmockextensionid0001");

    ExtensionShimBackend backend(id, QStringLiteral("tst-shim"), dir);

    // --- storage.local round trip + change records ----------------
    QJsonObject reply = backend.invoke(
        QStringLiteral("storage"), QStringLiteral("local.set"),
        QJsonObject{{QStringLiteral("items"),
                     QJsonObject{{QStringLiteral("a"), 1},
                                 {QStringLiteral("b"),
                                  QStringLiteral("x")}}}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    const QJsonObject changes = reply.value(QLatin1String("result"))
        .toObject().value(QLatin1String("changes")).toObject();
    QVERIFY(changes.value(QLatin1String("a")).toObject()
            .value(QLatin1String("newValue")).toInt() == 1);

    reply = backend.invoke(
        QStringLiteral("storage"), QStringLiteral("local.get"),
        QJsonObject{{QStringLiteral("keys"),
                     QStringLiteral("a")}});
    QCOMPARE(reply.value(QLatin1String("result")).toObject()
             .value(QLatin1String("a")).toInt(), 1);

    // get(null) returns the whole store; object keys merge defaults.
    reply = backend.invoke(
        QStringLiteral("storage"), QStringLiteral("local.get"),
        QJsonObject{{QStringLiteral("keys"), QJsonValue::Null}});
    QCOMPARE(reply.value(QLatin1String("result")).toObject().size(), 2);
    reply = backend.invoke(
        QStringLiteral("storage"), QStringLiteral("local.get"),
        QJsonObject{{QStringLiteral("keys"),
                     QJsonObject{{QStringLiteral("missing"),
                                  QStringLiteral("dflt")}}}});
    QCOMPARE(reply.value(QLatin1String("result")).toObject()
             .value(QLatin1String("missing")).toString(),
             QStringLiteral("dflt"));

    // local persists for the extension id across backend instances.
    {
        ExtensionShimBackend again(id, QStringLiteral("tst-shim"),
                                   dir);
        reply = again.invoke(
            QStringLiteral("storage"), QStringLiteral("local.get"),
            QJsonObject{{QStringLiteral("keys"),
                         QStringLiteral("b")}});
        QCOMPARE(reply.value(QLatin1String("result")).toObject()
                 .value(QLatin1String("b")).toString(),
                 QStringLiteral("x"));
    }

    // remove + clear emit oldValue change records.
    reply = backend.invoke(
        QStringLiteral("storage"), QStringLiteral("local.remove"),
        QJsonObject{{QStringLiteral("keys"),
                     QStringLiteral("a")}});
    QVERIFY(reply.value(QLatin1String("result")).toObject()
            .value(QLatin1String("changes")).toObject()
            .value(QLatin1String("a")).toObject()
            .contains(QLatin1String("oldValue")));
    reply = backend.invoke(
        QStringLiteral("storage"), QStringLiteral("local.clear"),
        QJsonObject());
    QVERIFY(reply.value(QLatin1String("result")).toObject()
            .value(QLatin1String("changes")).toObject()
            .contains(QLatin1String("b")));

    // --- storage.session is a separate in-memory area -------------
    QVERIFY(backend.invoke(
        QStringLiteral("storage"), QStringLiteral("session.set"),
        QJsonObject{{QStringLiteral("items"),
                     QJsonObject{{QStringLiteral("s"), true}}}})
        .value(QLatin1String("ok")).toBool());
    reply = backend.invoke(
        QStringLiteral("storage"), QStringLiteral("local.get"),
        QJsonObject{{QStringLiteral("keys"),
                     QStringLiteral("s")}});
    QVERIFY(!reply.value(QLatin1String("result")).toObject()
            .contains(QLatin1String("s")));
    reply = backend.invoke(
        QStringLiteral("storage"), QStringLiteral("session.get"),
        QJsonObject{{QStringLiteral("keys"),
                     QStringLiteral("s")}});
    QVERIFY(reply.value(QLatin1String("result")).toObject()
            .value(QLatin1String("s")).toBool());
    // Invalid areas and managed writes are typed errors, not no-ops.
    QVERIFY(!backend.invoke(
        QStringLiteral("storage"), QStringLiteral("bogus.get"),
        QJsonObject()).value(QLatin1String("ok")).toBool());
    reply = backend.invoke(
        QStringLiteral("storage"), QStringLiteral("managed.set"),
        QJsonObject{{QStringLiteral("items"), QJsonObject()}});
    QCOMPARE(reply.value(QLatin1String("error")).toString(),
             QStringLiteral("read-only"));

    // --- alarms ---------------------------------------------------
    QSignalSpy alarmSpy(&backend,
                        &ExtensionShimBackend::pushEvent);
    QVERIFY(backend.invoke(
        QStringLiteral("alarms"), QStringLiteral("create"),
        QJsonObject{{QStringLiteral("name"), QStringLiteral("al")},
                    {QStringLiteral("info"),
                     QJsonObject{{QStringLiteral("delayInMinutes"),
                                  0.002}}}})
        .value(QLatin1String("ok")).toBool());
    reply = backend.invoke(QStringLiteral("alarms"),
                           QStringLiteral("get"),
                           QJsonObject{{QStringLiteral("name"),
                                        QStringLiteral("al")}});
    QCOMPARE(reply.value(QLatin1String("result")).toObject()
             .value(QLatin1String("name")).toString(),
             QStringLiteral("al"));
    // The timer is real — onAlarm actually fires (~120ms).
    QTRY_VERIFY_WITH_TIMEOUT(alarmSpy.size() >= 1, 10000);
    QCOMPARE(alarmSpy.first().at(0).toString(),
             QStringLiteral("alarms.onAlarm"));
    QCOMPARE(alarmSpy.first().at(1).toJsonObject()
             .value(QLatin1String("name")).toString(),
             QStringLiteral("al"));

    // A cleared alarm never fires.
    QVERIFY(backend.invoke(
        QStringLiteral("alarms"), QStringLiteral("create"),
        QJsonObject{{QStringLiteral("name"), QStringLiteral("dead")},
                    {QStringLiteral("info"),
                     QJsonObject{{QStringLiteral("delayInMinutes"),
                                  0.002}}}})
        .value(QLatin1String("ok")).toBool());
    reply = backend.invoke(
        QStringLiteral("alarms"), QStringLiteral("clear"),
        QJsonObject{{QStringLiteral("name"),
                     QStringLiteral("dead")}});
    QCOMPARE(reply.value(QLatin1String("result")).toBool(), true);
    const int fired = alarmSpy.size();
    QTest::qWait(400);
    QCOMPARE(alarmSpy.size(), fired);

    // --- action state -----------------------------------------------
    QVERIFY(backend.invoke(
        QStringLiteral("action"), QStringLiteral("set"),
        QJsonObject{{QStringLiteral("key"), QStringLiteral("title")},
                    {QStringLiteral("value"),
                     QStringLiteral("T")}}).value("ok").toBool());
    QVERIFY(backend.invoke(
        QStringLiteral("browserAction"), QStringLiteral("set"),
        QJsonObject{{QStringLiteral("key"),
                     QStringLiteral("badgeText")},
                    {QStringLiteral("value"),
                     QStringLiteral("3")}}).value("ok").toBool());
    QVERIFY(backend.invoke(
        QStringLiteral("action"), QStringLiteral("set"),
        QJsonObject{{QStringLiteral("key"),
                     QStringLiteral("badgeColor")},
                    {QStringLiteral("value"),
                     QJsonArray{10, 20, 30, 128}}})
        .value(QLatin1String("ok")).toBool());
    const ExtensionShimBackend::ActionState state =
        ExtensionShimBackend::actionState(id);
    QCOMPARE(state.title, QStringLiteral("T"));
    QCOMPARE(state.badgeText, QStringLiteral("3"));
    QCOMPARE(state.badgeColor,
             QStringLiteral("rgba(10,20,30,%1)")
                 .arg(128.0 / 255.0, 0, 'f'));
    reply = backend.invoke(
        QStringLiteral("action"), QStringLiteral("get"),
        QJsonObject{{QStringLiteral("key"),
                     QStringLiteral("badgeColor")}});
    const QJsonArray rgba = reply.value(QLatin1String("result"))
        .toArray();
    QCOMPARE(rgba.at(0).toInt(), 10);
    QCOMPARE(rgba.at(3).toInt(), 128);
    // ExtensionInfo carries the action fields for the button strip.
    bool sawAction = false;
    for (const ExtensionManager::ExtensionInfo &info
         : ExtensionManager::instance()->extensions()) {
        if (info.id == id) {
            sawAction = info.actionTitle == QLatin1String("T")
                && info.actionBadgeText == QLatin1String("3");
        }
    }
    // The fixture was never loaded as a real extension — the info
    // row only exists when it is; the static registry check above is
    // the real assertion, this just must not crash.
    Q_UNUSED(sawAction);

    // --- i18n / permissions -----------------------------------------
    reply = backend.invoke(QStringLiteral("i18n"),
                           QStringLiteral("messages"),
                           QJsonObject());
    QCOMPARE(reply.value(QLatin1String("result")).toObject()
             .value(QLatin1String("hello")).toString(),
             QStringLiteral("Hello $1"));

    reply = backend.invoke(
        QStringLiteral("permissions"), QStringLiteral("contains"),
        QJsonObject{{QStringLiteral("permissions"),
                     QJsonArray{QStringLiteral("storage")}}});
    QCOMPARE(reply.value(QLatin1String("result")).toBool(), true);
    reply = backend.invoke(
        QStringLiteral("permissions"), QStringLiteral("contains"),
        QJsonObject{{QStringLiteral("permissions"),
                     QJsonArray{QStringLiteral("tabs")}}});
    QCOMPARE(reply.value(QLatin1String("result")).toBool(), false);
    reply = backend.invoke(QStringLiteral("permissions"),
                           QStringLiteral("request"),
                           QJsonObject());
    QCOMPARE(reply.value(QLatin1String("error")).toString(),
             QStringLiteral("unsupported"));

    // --- honest typed errors -----------------------------------------
    reply = backend.invoke(QStringLiteral("notifications"),
                           QStringLiteral("create"),
                           QJsonObject());
    QVERIFY(!reply.value(QLatin1String("ok")).toBool());
    QCOMPARE(reply.value(QLatin1String("error")).toString(),
             QStringLiteral("unsupported"));
    QVERIFY(backend.invoke(
        QStringLiteral("notifications"), QStringLiteral("getAll"),
        QJsonObject()).value(QLatin1String("ok")).toBool());
    reply = backend.invoke(QStringLiteral("bogus_ns"),
                           QStringLiteral("x"), QJsonObject());
    QCOMPARE(reply.value(QLatin1String("error")).toString(),
             QStringLiteral("unimplemented"));

    // --- diagnostics channel ------------------------------------------
    QSignalSpy gapSpy(&backend, &ExtensionShimBackend::gapReported);
    QVERIFY(backend.invoke(
        QStringLiteral("shim"), QStringLiteral("gap"),
        QJsonObject{{QStringLiteral("api"),
                     QStringLiteral("chrome.test.x")},
                    {QStringLiteral("error"),
                     QStringLiteral("unsupported")}})
        .value(QLatin1String("ok")).toBool());
    QCOMPARE(gapSpy.size(), 1);
    QVERIFY(backend.gaps().contains(
        QStringLiteral("chrome.test.x: unsupported")));
    QVERIFY(backend.invoke(
        QStringLiteral("shim"), QStringLiteral("note"),
        QJsonObject{{QStringLiteral("message"),
                     QStringLiteral("hello")}})
        .value(QLatin1String("ok")).toBool());
    QVERIFY(backend.notes().contains(QStringLiteral("hello")));

    // --- contextMenus / sidePanel registries --------------------------
    reply = backend.invoke(
        QStringLiteral("contextMenus"), QStringLiteral("create"),
        QJsonObject{{QStringLiteral("props"),
                     QJsonObject{{QStringLiteral("id"),
                                  QStringLiteral("mi")},
                                 {QStringLiteral("title"),
                                  QStringLiteral("Item")}}}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QCOMPARE(reply.value(QLatin1String("result")).toObject()
             .value(QLatin1String("id")).toString(),
             QStringLiteral("mi"));
    QVERIFY(!backend.invoke(
        QStringLiteral("contextMenus"), QStringLiteral("remove"),
        QJsonObject{{QStringLiteral("id"),
                     QStringLiteral("nope")}})
        .value(QLatin1String("ok")).toBool());

    // Clean the persisted store dir for the test id.
    QDir(BrowserPaths::dataFilePath(
        QStringLiteral("extstorage")) + QLatin1Char('/') + id)
        .removeRecursively();
    QDir(dir).removeRecursively();
}

// EXT15: the runtime relay — shimmed contexts register by extension
// id, send fan-out only reaches same-extension peers, and the first
// defined response routes back to the origin.  A peer that answers
// with nothing still resolves the send once all peers replied.
void tst_ExtensionManager::extensionMessageRelay()
{
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();
    const QString dir = QDir::temp().filePath(
        QStringLiteral("tst-extrelay-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dir));
    {
        QFile manifestFile(dir + QLatin1String("/manifest.json"));
        QVERIFY(manifestFile.open(QIODevice::WriteOnly));
        manifestFile.write(
            "{\"manifest_version\":3,\"name\":\"tst-relay\","
            "\"version\":\"1.0\"}");
        manifestFile.close();
    }

    // Two contexts on one extension + one on another extension.
    ChromeBridge bg(QStringLiteral("relayext0000000001"),
                    QStringLiteral("tst-relay"), dir);
    ChromeBridge popup(QStringLiteral("relayext0000000001"),
                       QStringLiteral("tst-relay"), dir);
    ChromeBridge other(QStringLiteral("relayext0000000002"),
                       QStringLiteral("tst-other"), dir);
    // Context ids carry a process-wide monotonic suffix — earlier
    // tests may have consumed slots, so compare re-registration
    // against the id the first call returned rather than a literal.
    const QString bgContextId = runtime->registerRelayContext(
        &bg, QStringLiteral("background"));
    QVERIFY(!bgContextId.isEmpty());
    QVERIFY(!runtime->registerRelayContext(
        &popup, QStringLiteral("popup")).isEmpty());
    QVERIFY(!runtime->registerRelayContext(
        &other, QStringLiteral("background")).isEmpty());
    // Re-registering the same bridge returns the same context id.
    QCOMPARE(runtime->registerRelayContext(
                 &bg, QStringLiteral("background")),
             bgContextId);

    QSignalSpy popupEvents(&popup, &ChromeBridge::hostEvent);
    QSignalSpy otherEvents(&other, &ChromeBridge::hostEvent);
    QSignalSpy bgEvents(&bg, &ChromeBridge::hostEvent);

    // send fans out to the same-extension peer only.
    const int peers = runtime->relaySend(
        &bg, QStringLiteral("m1"),
        QJsonObject{{QStringLiteral("__aroraMsg"),
                     QJsonObject{{QStringLiteral("mid"),
                                  QStringLiteral("m1")}}}});
    QCOMPARE(peers, 1);
    QCOMPARE(popupEvents.size(), 1);
    QCOMPARE(otherEvents.size(), 0);
    QCOMPARE(popupEvents.first().at(0).toString(),
             QStringLiteral("__arora.relay"));
    QCOMPARE(popupEvents.first().at(1).toJsonObject()
             .value(QLatin1String("mid")).toString(),
             QStringLiteral("m1"));

    // The peer's response routes back to the origin.
    QVERIFY(runtime->relayRespond(
        &popup, QStringLiteral("m1"),
        QJsonValue(QStringLiteral("pong"))));
    QCOMPARE(bgEvents.size(), 1);
    QCOMPARE(bgEvents.first().at(0).toString(),
             QStringLiteral("__arora.relayResponse"));
    QCOMPARE(bgEvents.first().at(1).toJsonObject()
             .value(QLatin1String("response")).toString(),
             QStringLiteral("pong"));

    // A spent mid no longer routes.
    QVERIFY(!runtime->relayRespond(
        &popup, QStringLiteral("m1"), QJsonValue()));

    // peers=0 message (no sibling contexts): nothing pending.
    QCOMPARE(runtime->relaySend(
                 &other, QStringLiteral("m2"), QJsonObject()),
             0);
    QVERIFY(!runtime->relayRespond(
        &other, QStringLiteral("m2"), QJsonValue(1)));

    // Empty peer reply resolves to null after all peers answered.
    QCOMPARE(runtime->relaySend(
                 &bg, QStringLiteral("m3"), QJsonObject()), 1);
    QVERIFY(runtime->relayRespond(
        &popup, QStringLiteral("m3"), QJsonValue()));
    QCOMPARE(bgEvents.size(), 2);
    QVERIFY(bgEvents.last().at(1).toJsonObject()
            .value(QLatin1String("response")).isNull());

    // Teardown unregisters; a further send finds no peers.
    runtime->unregisterRelayContext(&popup);
    QCOMPARE(runtime->relaySend(
                 &bg, QStringLiteral("m4"), QJsonObject()), 0);
    runtime->unregisterRelayContext(&bg);
    runtime->unregisterRelayContext(&other);

    QDir(dir).removeRecursively();
}

// EXT16: the event surfaces.  Two hosted extensions — one declaring
// tabs/webNavigation/contextMenus + a host permission, one declaring
// only storage — record real listener deliveries in-page; a real
// BrowserMainWindow drives tab create/activate/close and a data:
// navigation through the engine so the hub's tab and webNavigation
// events are exercised end to end.  The permission gate is asserted
// two ways: webNavigation deliveries never reach the unpermitted
// extension, and url-bearing tab payload fields are scrubbed for it
// (Chrome parity).
void tst_ExtensionManager::extensionEventSurfaces()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");

    const QString swRecord =
        "self.__extEvents=[];\n"
        "self.__extRec=function(n){return function(){\n"
        " self.__extEvents.push({n:n,a:Array.prototype.slice.call(\n"
        "  arguments).map(function(v){\n"
        "   try{return JSON.parse(JSON.stringify(v));}catch(e){\n"
        "    return String(v);}})});};};\n"
        "['onCreated','onUpdated','onActivated','onRemoved'].forEach(\n"
        " function(e){chrome.tabs[e].addListener(\n"
        "  self.__extRec('tabs.'+e));});\n"
        "['onBeforeNavigate','onCommitted','onCompleted',\n"
        " 'onErrorOccurred'].forEach(function(e){\n"
        " chrome.webNavigation[e].addListener(\n"
        "  self.__extRec('webNavigation.'+e));});\n"
        "chrome.contextMenus.onClicked.addListener(\n"
        " self.__extRec('contextMenus.onClicked'));\n";

    const QString dirA = QDir::temp().filePath(
        QStringLiteral("tst-extevA-%1")
            .arg(QCoreApplication::applicationPid()));
    const QString dirB = QDir::temp().filePath(
        QStringLiteral("tst-extevB-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dirA));
    QVERIFY(QDir().mkpath(dirB));
    {
        QFile f(dirA + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-events-a\","
                "\"version\":\"1.0\",\"permissions\":[\"storage\","
                "\"tabs\",\"webNavigation\",\"contextMenus\"],"
                "\"host_permissions\":[\"https://allowed.example/*\"],"
                "\"background\":{\"service_worker\":\"sw.js\"}}");
        f.close();
        QFile w(dirA + QLatin1String("/sw.js"));
        QVERIFY(w.open(QIODevice::WriteOnly));
        w.write(swRecord.toUtf8());
        w.close();
    }
    {
        QFile f(dirB + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-events-b\","
                "\"version\":\"1.0\",\"permissions\":[\"storage\"],"
                "\"background\":{\"service_worker\":\"sw.js\"}}");
        f.close();
        QFile w(dirB + QLatin1String("/sw.js"));
        QVERIFY(w.open(QIODevice::WriteOnly));
        w.write(swRecord.toUtf8());
        w.close();
    }

    ExtensionManager *manager = ExtensionManager::instance();
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();
    ExtensionEventHub *hub = ExtensionEventHub::instance();

    const auto loadReady = [this, manager, runtime](
            const QString &dir, const QString &name) -> QString {
        auto loaded = std::make_shared<QString>();
        QObject::connect(manager, &ExtensionManager::extensionLoaded,
                         this, [loaded, name](
                             const ExtensionManager::ExtensionInfo
                                 &info) {
            if (info.name == name && info.error.isEmpty())
                *loaded = info.id;
        });
        manager->grantConsent(dir);
        manager->loadExtension(dir);
        QElapsedTimer timer;
        timer.start();
        while (loaded->isEmpty() && timer.elapsed() < 30000)
            QTest::qWait(50);
        const QString id = *loaded;
        if (id.isEmpty())
            return id;
        manager->setExtensionEnabled(id, true);
        timer.restart();
        while (timer.elapsed() < 45000) {
            ExtensionBackgroundHost *host = runtime->host(id);
            if (host && host->state()
                    == ExtensionBackgroundHost::State::Ready)
                break;
            QTest::qWait(50);
        }
        return id;
    };
    const QString idA = loadReady(dirA,
                                  QStringLiteral("tst-events-a"));
    QVERIFY2(!idA.isEmpty(), "fixture A loaded + hosted");
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime->host(idA)
            && runtime->host(idA)->state()
               == ExtensionBackgroundHost::State::Ready,
        45000);
    const QString idB = loadReady(dirB,
                                  QStringLiteral("tst-events-b"));
    QVERIFY2(!idB.isEmpty(), "fixture B loaded + hosted");
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime->host(idB)
            && runtime->host(idB)->state()
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
    auto sawEvent = [&eval](QWebEnginePage *page, const QString &name) {
        return eval(page, QStringLiteral(
            "(self.__extEvents||[]).some(function(e){return e.n===\"")
            + name + QLatin1String("\";})"))
            .toBool();
    };

    // --- hub tracking + window/tab drive -----------------------------
    hub->setTracking(true);
    hub->syncNow();   // silent baseline

    BrowserMainWindow *window = new BrowserMainWindow;
    TabWidget *tw = window->tabWidget();
    QVERIFY(tw && tw->count() >= 1);
    hub->syncNow();
    QVERIFY(hub->trackedTabCount() >= 1);
    QVERIFY(ExtensionEventHub::browsingWindows().contains(window));
    QVERIFY(ExtensionEventHub::trackable(tw->currentWebView()));

    // The window's first tab was born after tracking began ->
    // onCreated lands in-page (A and B both — tabs events are
    // ungated).
    QTRY_VERIFY_WITH_TIMEOUT(
        sawEvent(runtime->host(idA)->page(),
                 QStringLiteral("tabs.onCreated")),
        20000);

    // A second tab + activation + close cover the remaining tab
    // events.
    tw->newTab();
    hub->syncNow();
    QVERIFY(tw->count() >= 2);
    tw->setCurrentIndex(0);
    WebView *view = tw->webView(0);
    QVERIFY(view);

    // A real navigation — the webNavigation lifecycle and
    // tabs.onUpdated (loading -> complete) ride the page signals.
    view->load(QUrl(QStringLiteral("data:text/html,<title>ev</title>ok")));
    tw->closeTab(1);
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    hub->syncNow();

    QTRY_VERIFY_WITH_TIMEOUT(
        sawEvent(runtime->host(idA)->page(),
                 QStringLiteral("tabs.onActivated"))
            && sawEvent(runtime->host(idA)->page(),
                        QStringLiteral("tabs.onUpdated"))
            && sawEvent(runtime->host(idA)->page(),
                        QStringLiteral("tabs.onRemoved"))
            && sawEvent(runtime->host(idA)->page(),
                        QStringLiteral("webNavigation.onCommitted")),
        30000);
    QTRY_VERIFY_WITH_TIMEOUT(
        sawEvent(runtime->host(idA)->page(),
                 QStringLiteral("webNavigation.onCompleted"))
            || sawEvent(runtime->host(idA)->page(),
                        QStringLiteral("webNavigation.onErrorOccurred")),
        15000);

    // Payload shape — the onUpdated entry carries
    // (tabId, changeInfo, tab); the committed entry carries
    // tabId/url/frameId.
    {
        const QJsonArray rows = QJsonDocument::fromJson(
            eval(runtime->host(idA)->page(), QStringLiteral(
                "JSON.stringify(self.__extEvents)"))
                .toString().toUtf8()).array();
        bool sawUpdatedShape = false;
        bool sawCommittedShape = false;
        for (const QJsonValue &v : rows) {
            const QJsonObject e = v.toObject();
            const QString n = e.value(QLatin1String("n")).toString();
            const QJsonArray a = e.value(QLatin1String("a")).toArray();
            if (n == QLatin1String("tabs.onUpdated") && a.size() == 3
                    && a.at(2).toObject()
                           .contains(QLatin1String("url")))
                sawUpdatedShape = true;
            if (n == QLatin1String("webNavigation.onCommitted")
                    && a.size() == 1
                    && a.at(0).toObject()
                           .contains(QLatin1String("tabId"))
                    && a.at(0).toObject()
                           .contains(QLatin1String("url"))
                    && a.at(0).toObject()
                           .value(QLatin1String("frameId")).toInt(-1)
                           == 0)
                sawCommittedShape = true;
        }
        QVERIFY2(sawUpdatedShape,
                 "tabs.onUpdated(tabId, changeInfo, tab) shape");
        QVERIFY2(sawCommittedShape,
                 "webNavigation.onCommitted(details) shape");
    }

    // --- permission gating -------------------------------------------
    // B declares no webNavigation permission — the whole family is
    // gated for it.  tabs.* events do arrive (ungated) but url-bearing
    // fields are scrubbed ('tabs' permission and no host match).
    const QVariant bNav = eval(runtime->host(idB)->page(),
        QStringLiteral(
            "(self.__extEvents||[]).some(function(e){"
            " return e.n.indexOf('webNavigation.')===0;})"));
    QCOMPARE(bNav.toBool(), false);
    const QVariant bScrub = eval(runtime->host(idB)->page(),
        QStringLiteral(
            "(self.__extEvents||[]).filter(function(e){"
            " return e.n==='tabs.onUpdated';}).every(function(e){"
            " var t=e.a[2]||{}; return t.url===undefined"
            "   &&t.title===undefined"
            "   &&(e.a[1]||{}).url===undefined;})"));
    QCOMPARE(bScrub.toBool(), true);
    // B still sees the events themselves.
    QVERIFY(sawEvent(runtime->host(idB)->page(),
                     QStringLiteral("tabs.onUpdated")));

    // A keeps the url — 'tabs' permission grants it.
    const QVariant aUrl = eval(runtime->host(idA)->page(),
        QStringLiteral(
            "(self.__extEvents||[]).some(function(e){"
            " return e.n==='tabs.onUpdated'&&e.a[2]"
            "   &&e.a[2].url!==undefined;})"));
    QCOMPARE(aUrl.toBool(), true);

    // --- contextMenus: registry -> real menu -> onClicked -------------
    ExtensionShimBackend *backendA =
        runtime->host(idA)->bridge()->backend();
    QVERIFY(backendA);
    QVERIFY(backendA->invoke(
        QStringLiteral("contextMenus"), QStringLiteral("create"),
        QJsonObject{{QStringLiteral("props"),
                     QJsonObject{{QStringLiteral("id"),
                                  QStringLiteral("mi")},
                                 {QStringLiteral("title"),
                                  QStringLiteral("Ev Item")},
                                 {QStringLiteral("contexts"),
                                  QJsonArray{QStringLiteral("page")}}}}})
        .value(QLatin1String("ok")).toBool());
    QCOMPARE(ExtensionShimBackend::contextMenuEntries().size(), 1);

    QMenu menu;
    Engine::ContextMenuInfo cinfo;
    ExtensionEventHub::contributeContextMenu(&menu, cinfo, view);
    bool sawItem = false;
    for (QAction *action : menu.actions()) {
        if (QMenu *sub = action->menu()) {
            for (QAction *item : sub->actions()) {
                if (item->text() == QLatin1String("Ev Item"))
                    sawItem = true;
            }
        }
    }
    QVERIFY2(sawItem, "registered item renders in the page menu");

    ExtensionEventHub::deliverMenuClicked(idA, QStringLiteral("mi"),
                                          cinfo, view);
    QTRY_VERIFY_WITH_TIMEOUT(
        sawEvent(runtime->host(idA)->page(),
                 QStringLiteral("contextMenus.onClicked")),
        20000);
    // Targeted delivery — B's identical listener stays silent.
    QCOMPARE(eval(runtime->host(idB)->page(), QStringLiteral(
        "(self.__extEvents||[]).some(function(e){"
        " return e.n==='contextMenus.onClicked';})")).toBool(), false);
    // The click info reports the menuItemId.
    QCOMPARE(eval(runtime->host(idA)->page(), QStringLiteral(
        "(self.__extEvents||[]).some(function(e){"
        " return e.n==='contextMenus.onClicked'&&e.a[0]"
        "   &&e.a[0].menuItemId==='mi';})")).toBool(), true);

    // --- fan-out gate, direct-drive -----------------------------------
    ChromeBridge *bridgeA = runtime->host(idA)->bridge();
    ChromeBridge *bridgeB = runtime->host(idB)->bridge();
    QSignalSpy spyA(bridgeA, &ChromeBridge::hostEvent);
    QSignalSpy spyB(bridgeB, &ChromeBridge::hostEvent);
    runtime->broadcastEvent(
        QStringLiteral("webNavigation.onCommitted"),
        QJsonObject{{QStringLiteral("tabId"), 1},
                    {QStringLiteral("url"),
                     QStringLiteral("https://x.example/")}});
    QCOMPARE(spyA.size(), 1);
    QCOMPARE(spyB.size(), 0);
    runtime->deliverEvent(
        idA, QStringLiteral("contextMenus.onClicked"),
        QJsonObject{{QStringLiteral("menuItemId"),
                     QStringLiteral("mi")}});
    QCOMPARE(spyA.size(), 2);
    QCOMPARE(spyB.size(), 0);

    // Cleanup — the menu registry dies with the backend on teardown.
    window->close();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    manager->setExtensionEnabled(idA, false);
    manager->setExtensionEnabled(idB, false);
    QTRY_VERIFY_WITH_TIMEOUT(
        !runtime->host(idA) && !runtime->host(idB), 20000);
    hub->setTracking(false);
    manager->removeExtension(idA);
    manager->removeExtension(idB);
    QDir(dirA).removeRecursively();
    QDir(dirB).removeRecursively();
}

// EXT17: the sibling-page context.  A fixture extension declaring an
// action popup gets its popup document loaded in a second
// QWebEngineView wired through attachExtensionPage — the page
// injection must land the shim there, the popup's chrome.runtime.
// sendMessage must reach the hosted background over the relay and
// back, getContexts must list both contexts, and teardown must close
// the popup context.
void tst_ExtensionManager::extensionPopupContext()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");

    const QString dir = QDir::temp().filePath(
        QStringLiteral("tst-extpopup-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dir));
    {
        QFile f(dir + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-popup\","
                "\"version\":\"1.0\",\"permissions\":[\"storage\"],"
                "\"action\":{\"default_popup\":\"popup.html\","
                "\"default_title\":\"Pop\"},"
                "\"background\":{\"service_worker\":\"sw.js\"}}");
        f.close();
        QFile w(dir + QLatin1String("/sw.js"));
        QVERIFY(w.open(QIODevice::WriteOnly));
        w.write(
            "chrome.runtime.onInstalled.addListener(function(){});\n"
            "self.__sawPopupPing=false;\n"
            "chrome.runtime.onMessage.addListener("
            "function(m,s,r){ if(m&&m.ping){"
            " self.__sawPopupPing=true;"
            " r({pong:true,from:'hosted'}); return true; }"
            " return false; });\n");
        w.close();
        QFile h(dir + QLatin1String("/popup.html"));
        QVERIFY(h.open(QIODevice::WriteOnly));
        h.write("<!doctype html><html><body><div id=\"r\">p</div>"
                "<script src=\"popup.js\"></script></body></html>");
        h.close();
        QFile j(dir + QLatin1String("/popup.js"));
        QVERIFY(j.open(QIODevice::WriteOnly));
        j.write("self.__popupBootRan=!!(chrome&&chrome.runtime);\n");
        j.close();
    }

    ExtensionManager *manager = ExtensionManager::instance();
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();
    auto loaded = std::make_shared<QString>();
    QObject::connect(manager, &ExtensionManager::extensionLoaded,
                     this, [loaded](
                         const ExtensionManager::ExtensionInfo &info) {
        if (info.name == QLatin1String("tst-popup")
                && info.error.isEmpty())
            *loaded = info.id;
    });
    manager->grantConsent(dir);
    manager->loadExtension(dir);
    QElapsedTimer timer;
    timer.start();
    while (loaded->isEmpty() && timer.elapsed() < 30000)
        QTest::qWait(50);
    const QString id = *loaded;
    QVERIFY2(!id.isEmpty(), "popup fixture loaded");
    manager->setExtensionEnabled(id, true);
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime->host(id)
            && runtime->host(id)->state()
               == ExtensionBackgroundHost::State::Ready,
        45000);
    ExtensionBackgroundHost *host = runtime->host(id);
    QVERIFY(host && host->page() && host->page()->profile());

    auto eval = [](QWebEnginePage *page, const QString &source) {
        auto result = std::make_shared<QVariant>();
        page->runJavaScript(source,
                            [result](const QVariant &v) {
                                *result = v; });
        QElapsedTimer t;
        t.start();
        while (!result->isValid() && t.elapsed() < 15000)
            QTest::qWait(50);
        return *result;
    };

    // --- attach + load the popup document -------------------------
    auto *popupView = new QWebEngineView;
    QWebEnginePage *popup =
        new QWebEnginePage(host->page()->profile(), popupView);
    popupView->setPage(popup);
    // The bridge goes on BEFORE the document so the DocumentCreation
    // preamble's channel poll finds it.
    QVERIFY(runtime->attachExtensionPage(popup, id));
    popup->load(QUrl(QStringLiteral("chrome-extension://") + id
                     + QStringLiteral("/popup.html")));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(popup, QStringLiteral(
            "!!(window.__aroraHost&&__aroraHost.bridgeUp"
            "&&window.__aroraShim)")).toBool(),
        30000);
    QCOMPARE(eval(popup, QStringLiteral(
                 "self.__popupBootRan===true")).toBool(),
             true);
    // The page injection skipped the background loader — the
    // document marks itself 'page'-kind.
    QCOMPARE(eval(popup, QStringLiteral(
                 "window.__aroraHost.kind")).toString(),
             QStringLiteral("page"));

    // --- popup -> hosted background sendMessage --------------------
    eval(popup, QStringLiteral(
        "self.__pr=undefined;"
        "chrome.runtime.sendMessage({ping:true},function(r){"
        "self.__pr=r===undefined?{undef:true}:r;});0"));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(popup, QStringLiteral("self.__pr!==undefined"))
            .toBool(),
        20000);
    QCOMPARE(eval(popup, QStringLiteral(
                 "self.__pr&&self.__pr.pong===true"
                 "&&self.__pr.from==='hosted'")).toBool(),
             true);
    // The hosted context itself saw the delivery (relay path).
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(host->page(), QStringLiteral(
            "self.__sawPopupPing===true")).toBool(),
        20000);

    // --- getContexts / tabs.getCurrent / windows -------------------
    eval(popup, QStringLiteral(
        "self.__gc=undefined;"
        "chrome.runtime.getContexts().then(function(v){"
        "self.__gc=v;});0"));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(popup, QStringLiteral("self.__gc!==undefined"))
            .toBool(),
        15000);
    const QJsonArray contexts = QJsonDocument::fromJson(
        QStringLiteral("[%1]")
            .arg(eval(popup, QStringLiteral(
                "JSON.stringify(self.__gc)")).toString())
            .toUtf8()).array().first().toArray();
    {
        bool sawBackground = false, sawPopup = false;
        for (const QJsonValue &c : contexts) {
            const QString t = c.toObject()
                .value(QLatin1String("contextType")).toString();
            if (t == QLatin1String("BACKGROUND"))
                sawBackground = true;
            if (t == QLatin1String("POPUP"))
                sawPopup = true;
        }
        QVERIFY2(sawBackground, "getContexts lists BACKGROUND");
        QVERIFY2(sawPopup, "getContexts lists POPUP");
    }
    // A detached popup answers undefined for tabs.getCurrent.
    eval(popup, QStringLiteral(
        "self.__ct=undefined;"
        "chrome.tabs.getCurrent().then(function(t){"
        "self.__ct=(t===undefined)?'undef':'tab';});0"));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(popup, QStringLiteral("self.__ct!==undefined"))
            .toBool(),
        15000);
    QCOMPARE(eval(popup, QStringLiteral("self.__ct")).toString(),
             QStringLiteral("undef"));

    // --- EXT19: the live action-bar path — a plain view + page on
    // the profile must self-wire: no explicit attachExtensionPage,
    // the runtime's view watch attaches the channel once the
    // chrome-extension document commits. --------------------------
    auto *liveFrame = new QFrame;
    auto *liveView = new QWebEngineView(liveFrame);
    QWebEnginePage *livePage =
        new QWebEnginePage(host->page()->profile(), liveView);
    liveView->setPage(livePage);
    livePage->load(QUrl(QStringLiteral("chrome-extension://") + id
                        + QStringLiteral("/popup.html")));
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime->hasExtensionPage(livePage), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(livePage, QStringLiteral(
            "!!(window.__aroraHost&&__aroraHost.bridgeUp"
            "&&window.__aroraShim)")).toBool(),
        30000);
    QCOMPARE(eval(livePage, QStringLiteral(
                 "self.__popupBootRan===true")).toBool(),
             true);
    // Navigating away from the extension document drops the bridge.
    livePage->load(QUrl(QStringLiteral("about:blank")));
    QTRY_VERIFY_WITH_TIMEOUT(
        !runtime->hasExtensionPage(livePage), 15000);
    liveView->deleteLater();
    liveFrame->deleteLater();
    // The watch sweeps the host's hidden view too, but a page bridge
    // must never land on the host's own document — it would replace
    // the channel the host preamble already connected to.
    QVERIFY(!runtime->hasExtensionPage(host->page()));

    // --- teardown ---------------------------------------------------
    popupView->deleteLater();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QTRY_VERIFY_WITH_TIMEOUT(
        !runtime->contextsJson(id).isEmpty(), 5000);   // bg stays
    {
        bool popupGone = true;
        for (const QJsonValue &c : runtime->contextsJson(id)) {
            if (c.toObject().value(QLatin1String("contextType"))
                    .toString() == QLatin1String("POPUP"))
                popupGone = false;
        }
        QVERIFY2(popupGone, "popup context closed with the page");
    }

    manager->setExtensionEnabled(id, false);
    QTRY_VERIFY_WITH_TIMEOUT(!runtime->host(id), 20000);
    manager->removeExtension(id);
    QDir(dir).removeRecursively();
}

QTEST_MAIN(tst_ExtensionManager)
#include "tst_extensionmanager.moc"
