/*
 * Copyright 2026 The Arktos Authors
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

#include <qcheckbox.h>
#include <qlistwidget.h>
#include <qpushbutton.h>

#include "browserapplication.h"
#include "browsermainwindow.h"
#include "browserpaths.h"
#include "browserprofile.h"
#include "extensionbackgroundhost.h"
#include "extensioneventhub.h"
#include "extensionmanager.h"
#include "extensionpagebridge.h"
#include "extensionrequestwatcher.h"
#include "extensionreviewdialog.h"
#include "extensionshimbackend.h"
#include "engineinterface.h"
#include "profilesettings.h"
#include "qtest_arktos.h"
#include "qtry.h"
#include "tabwidget.h"
#include "webpage.h"
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
    void integrityPin();
    void broadHostWarning();
    void updateDriftReview();
    void healthClassification();
    void backgroundHealthProbe();
    void hostedBackgroundRuntime();
    void extensionShimBackend();
    void extensionMessageRelay();
    void extensionEventSurfaces();
    void extensionWebRequest();
    void extensionRuntimeLifecycle();
    void extensionPopupContext();
    void extensionPageAgent();
    void extensionTabMessaging();
    void extensionScripting();
    void extensionScriptCss();
    void extensionContentScriptRegistration();
    void extensionOffscreen();
    void actionPopupSelfWires();
    void realExtensionPopup();
    void bitwardenGapBaseline();
    void bitwardenHandshakeGate();
    void enabledStatePersists();

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

    // Plain recursive file copy for the ER00 fixture — the real
    // bundle is read-only input; the leg installs from this copy.
    static bool copyTree(const QString &from, const QString &to) {
        const QDir src(from);
        if (!src.exists() || !QDir().mkpath(to))
            return false;
        const QFileInfoList entries = src.entryInfoList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
        for (const QFileInfo &e : entries) {
            const QString dst = to + QLatin1Char('/') + e.fileName();
            if (e.isDir()) {
                if (!copyTree(e.absoluteFilePath(), dst))
                    return false;
            } else if (e.isFile()
                       && !QFile::copy(e.absoluteFilePath(), dst)) {
                return false;
            }
        }
        return true;
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
    QTest::addColumn<QStringList>("fatal");
    QTest::addColumn<QStringList>("dangerous");
    QTest::addColumn<QStringList>("dangerousHosts");

    QTest::newRow("minimal mv3")
        << QByteArray("{\"manifest_version\":3,\"name\":\"mini\","
                      "\"version\":\"1.0\"}")
        << true << 3 << QStringLiteral("mini") << false
        << QStringList() << QStringList() << QStringList()
        << QStringList() << QStringList();
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
        << (QStringList() << QStringLiteral("webRequest"))
        << (QStringList() << QStringLiteral("cookies")
                          << QStringLiteral("nativeMessaging"))
        << (QStringList() << QStringLiteral("<all_urls>"));
    // EXT21: required scripting marks the extension's purpose dead —
    // the Bitwarden signature.  An OPTIONAL scripting grant must not.
    QTest::newRow("fatal core api required")
        << QByteArray("{\"manifest_version\":3,\"name\":\"vault\","
                      "\"version\":\"1.0\","
                      "\"permissions\":[\"scripting\",\"storage\"]}")
        << true << 3 << QStringLiteral("vault") << false
        << QStringList()
        << (QStringList() << QStringLiteral("scripting"))
        << (QStringList() << QStringLiteral("scripting"))
        << QStringList() << QStringList();
    QTest::newRow("fatal core api optional")
        << QByteArray("{\"manifest_version\":3,\"name\":\"maybe\","
                      "\"version\":\"1.0\","
                      "\"permissions\":[\"storage\"],"
                      "\"optional_permissions\":[\"scripting\"]}")
        << true << 3 << QStringLiteral("maybe") << false
        << QStringList()
        << (QStringList() << QStringLiteral("scripting"))
        << QStringList()
        << QStringList() << QStringList();
    QTest::newRow("background flag")
        << QByteArray("{\"manifest_version\":3,\"name\":\"bg\","
                      "\"version\":\"1.0\",\"background\":"
                      "{\"service_worker\":\"sw.js\"}}")
        << true << 3 << QStringLiteral("bg") << true
        << QStringList() << QStringList() << QStringList()
        << QStringList() << QStringList();
    QTest::newRow("not json")
        << QByteArray("this is not json")
        << false << 0 << QString() << false
        << QStringList() << QStringList() << QStringList()
        << QStringList() << QStringList();
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
    QFETCH(QStringList, fatal);
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
        // EXT21: fatalMissing is an exact match — an over-wide set
        // would condemn working extensions.
        QCOMPARE(parsed.fatalMissing, fatal);
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
    QVERIFY(!ExtensionManager::instance()->updatesPath().isEmpty());
    QVERIFY(QDir(ExtensionManager::instance()->updatesPath()).exists());
    QVERIFY(!ExtensionManager::instance()->userScriptsPath().isEmpty());
    // EXT05: the userscript store must be owner-only.
    QVERIFY(!(QFileInfo(ExtensionManager::instance()->userScriptsPath())
                  .permissions()
              & (QFileDevice::ReadGroup | QFileDevice::WriteGroup
                 | QFileDevice::ExeGroup | QFileDevice::ReadOther
                 | QFileDevice::WriteOther | QFileDevice::ExeOther)));

    const bool saved = ExtensionManager::instance()->updateCheckEnabled();
    ExtensionManager::instance()->setUpdateCheckEnabled(!saved);
    QCOMPARE(ExtensionManager::instance()->updateCheckEnabled(), !saved);
    ExtensionManager::instance()->setUpdateCheckEnabled(saved);
    QCOMPARE(ExtensionManager::instance()->updateCheckEnabled(), saved);
}

// EXT06: earlier legs of this test installed into the user's real
// "arktos" profile when the binary ran outside runTests.sh (which
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
                     + QLatin1String("/.local/share/Arktos")),
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

    qputenv("ARKTOS_BUNDLED_EXTENSIONS_DIR", root.toUtf8());
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
        ProfileSettings settings;
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
    const QStringList consumed = ProfileSettings().value(
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

    qputenv("ARKTOS_BUNDLED_EXTENSIONS_DIR", QByteArray());
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
    QVERIFY(resA.savedTo.startsWith(ExtensionManager::instance()->updatesPath()));
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

// SEC25 (a): every successful install pins a sha256 of its on-disk
// tree; the startup sweep re-hashes and flags a mismatch instead of
// silently loading — "files changed since install".
void tst_ExtensionManager::integrityPin()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");

    const QString root = QDir::temp().filePath(
        QStringLiteral("tst-extpin-%1")
            .arg(QCoreApplication::applicationPid()));
    const QString dir = writeExtension(root, QStringLiteral("tst-pin"),
                                       QStringLiteral("1.0.0"));
    QVERIFY(!dir.isEmpty());

    ExtensionManager *manager = ExtensionManager::instance();
    QWebEngineProfile *profile = BrowserApplication::webEngineProfile();

    // Sweep earlier leftovers by manifest name (pid-rooted ids differ
    // per run, the manifest name survives into the installed copy).
    for (const ExtensionManager::ExtensionInfo &info
         : manager->extensions()) {
        if (!info.builtin && info.name == QLatin1String("tst-pin"))
            manager->removeExtension(info.id);
    }
    QTest::qWait(300);

    const QString id = installFixture(dir);
    QVERIFY2(!id.isEmpty(), "fixture extension must install");

    const auto infoFor = [manager](const QString &extId) {
        const QList<ExtensionManager::ExtensionInfo> list =
            manager->extensions();
        for (const ExtensionManager::ExtensionInfo &info : list)
            if (info.id == extId)
                return info;
        return ExtensionManager::ExtensionInfo();
    };

    // The install pinned the tree — a sweep right after is clean.
    auto tampered = std::make_shared<QStringList>();
    QObject::connect(manager, &ExtensionManager::extensionTampered,
                     this, [tampered](
                         const ExtensionManager::ExtensionInfo &info) {
        tampered->append(info.id);
    });
    manager->verifyInstalledIntegrity(profile);
    QVERIFY(tampered->isEmpty());
    QVERIFY(!infoFor(id).tampered);

    // Tamper a file in the installed dir -> the next sweep flags it.
    const QString installedDir = infoFor(id).path;
    QVERIFY(!installedDir.isEmpty());
    QFile victim(installedDir + QLatin1String("/sw.js"));
    QVERIFY(victim.open(QIODevice::Append));
    victim.write("// tampered\n");
    victim.close();

    manager->verifyInstalledIntegrity(profile);
    QVERIFY(tampered->contains(id));
    QVERIFY(infoFor(id).tampered);

    // The hash itself: deterministic over a tree, sensitive to a new
    // file, empty for a missing dir.
    const QByteArray h1 = ExtensionManager::extensionTreeHash(dir);
    QVERIFY(!h1.isEmpty());
    QCOMPARE(ExtensionManager::extensionTreeHash(dir), h1);
    QFile extra(dir + QLatin1String("/extra.js"));
    QVERIFY(extra.open(QIODevice::WriteOnly));
    extra.write("x");
    extra.close();
    QVERIFY(ExtensionManager::extensionTreeHash(dir) != h1);
    QVERIFY(ExtensionManager::extensionTreeHash(
                QStringLiteral("/nonexistent/ext-dir")).isEmpty());

    // Removing clears the flag with the extension.
    manager->removeExtension(id);
    QTest::qWait(500);
    QVERIFY(!infoFor(id).tampered);   // row gone -> default info

    QDir(root).removeRecursively();
}

// SEC25 (c): whole-scheme host permissions (<all_urls>, *://*/*,
// http/https wildcard roots) are visually escalated in the review
// dialog — a warning icon on the permission row plus a dedicated
// warnings row — while scoped wildcards stay plain.
void tst_ExtensionManager::broadHostWarning()
{
    QVERIFY(ExtensionManager::isBroadHostPattern(
                QLatin1String("<all_urls>")));
    QVERIFY(ExtensionManager::isBroadHostPattern(
                QLatin1String("*://*/*")));
    QVERIFY(ExtensionManager::isBroadHostPattern(
                QLatin1String("http://*/*")));
    QVERIFY(ExtensionManager::isBroadHostPattern(
                QLatin1String("https://*/*")));
    QVERIFY(!ExtensionManager::isBroadHostPattern(
                QLatin1String("*://*.example.com/*")));
    QVERIFY(!ExtensionManager::isBroadHostPattern(
                QLatin1String("https://example.com/*")));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QFile file(dir.path() + QLatin1String("/manifest.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{\"manifest_version\":3,\"name\":\"Broad\","
                   "\"version\":\"1.0\",\"host_permissions\":"
                   "[\"https://*/*\",\"*://*.example.com/*\"]}");
    }
    const ExtensionManager::Manifest manifest =
        ExtensionManager::inspectManifest(dir.path());
    QVERIFY(manifest.valid);

    ExtensionReviewDialog dialog(manifest, dir.path(),
                                 ExtensionReviewDialog::Install);
    QListWidget *warnings = dialog.findChild<QListWidget *>(
        QLatin1String("warningsList"));
    QVERIFY(warnings);
    QString warningText;
    for (int i = 0; i < warnings->count(); ++i)
        warningText += warnings->item(i)->text() + QLatin1Char('\n');
    QVERIFY(warningText.contains(QLatin1String("Broad host access")));
    QVERIFY(warningText.contains(QLatin1String("https://*/*")));
    // The scoped subdomain wildcard is not escalated.
    QVERIFY(!warningText.contains(QLatin1String("example.com")));

    // The broad row carries the warning icon; the scoped row none.
    QListWidget *perms = dialog.findChild<QListWidget *>(
        QLatin1String("permissionsList"));
    QVERIFY(perms);
    bool broadHasIcon = false;
    bool scopedPlain = false;
    for (int i = 0; i < perms->count(); ++i) {
        QListWidgetItem *item = perms->item(i);
        if (item->toolTip() == QLatin1String("https://*/*"))
            broadHasIcon = !item->icon().isNull();
        if (item->toolTip() == QLatin1String("*://*.example.com/*"))
            scopedPlain = item->icon().isNull();
    }
    QVERIFY(broadHasIcon);
    QVERIFY(scopedPlain);
}

// SEC25 (b): applying an update re-presents grants the installed
// version never had — added permissions and host patterns listed as
// "new since installed version", escalated dangerous access called
// out and still gated on the per-item consent checkboxes.
void tst_ExtensionManager::updateDriftReview()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QFile file(dir.path() + QLatin1String("/manifest.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{\"manifest_version\":3,\"name\":\"Grown\","
                   "\"version\":\"2.0\",\"permissions\":"
                   "[\"tabs\",\"cookies\"],"
                   "\"host_permissions\":[\"<all_urls>\"]}");
    }
    const ExtensionManager::Manifest next =
        ExtensionManager::inspectManifest(dir.path());
    QVERIFY(next.valid);

    // The installed version declared "tabs" only, no host access.
    ExtensionManager::Manifest installed;
    installed.valid = true;
    installed.manifestVersion = 3;
    installed.name = QLatin1String("Grown");
    installed.version = QLatin1String("1.0");
    installed.permissions = {QStringLiteral("tabs")};

    ExtensionReviewDialog dialog(next, dir.path(),
                                 ExtensionReviewDialog::Install,
                                 nullptr, &installed);
    QListWidget *warnings = dialog.findChild<QListWidget *>(
        QLatin1String("warningsList"));
    QVERIFY(warnings);
    QString warningText;
    for (int i = 0; i < warnings->count(); ++i)
        warningText += warnings->item(i)->text() + QLatin1Char('\n');
    QVERIFY(warningText.contains(QLatin1String("New since")));
    QVERIFY(warningText.contains(QLatin1String("1.0")));
    QVERIFY(warningText.contains(QLatin1String("cookies")));
    QVERIFY(warningText.contains(QLatin1String("<all_urls>")));
    QVERIFY(warningText.contains(QLatin1String("high-risk")));
    // Pre-existing grants are not re-listed as new.
    QVERIFY(!warningText.contains(
                QRegularExpression(QStringLiteral("New since.*tabs"))));

    // Escalated dangerous grants still need explicit per-item consent.
    QPushButton *approve = dialog.findChild<QPushButton *>(
        QLatin1String("approveButton"));
    QVERIFY(approve);
    QVERIFY(!approve->isEnabled());
    const QList<QCheckBox *> consents =
        dialog.findChildren<QCheckBox *>(
            QLatin1String("dangerousConsent"));
    QCOMPARE(consents.size(), next.dangerous.size());

    // No installed baseline (initial install) -> no drift rows.
    ExtensionReviewDialog fresh(next, dir.path(),
                                ExtensionReviewDialog::Install);
    QListWidget *freshWarnings = fresh.findChild<QListWidget *>(
        QLatin1String("warningsList"));
    QVERIFY(freshWarnings);
    QString freshText;
    for (int i = 0; i < freshWarnings->count(); ++i)
        freshText += freshWarnings->item(i)->text() + QLatin1Char('\n');
    QVERIFY(!freshText.contains(QLatin1String("New since")));
}

// EXT12: the degradation classifier folds declared-but-missing
// chrome.* APIs and the measured background verdict into one row
// state — pin the whole matrix so a regression in the fold is loud.
void tst_ExtensionManager::healthClassification()
{
    using EH = ExtensionManager::ExtensionHealth;
    using PV = ExtensionManager::ProbeVerdict;

    const QStringList none;
    QCOMPARE(ExtensionManager::classifyHealth(
                 false, none, none, PV::Pending),
             EH::Unknown);
    QCOMPARE(ExtensionManager::classifyHealth(
                 false, none, none, PV::None),
             EH::Healthy);
    QCOMPARE(ExtensionManager::classifyHealth(
                 true, none, none, PV::Pending),
             EH::Unknown);
    QCOMPARE(ExtensionManager::classifyHealth(
                 true, none, none, PV::Responsive),
             EH::Healthy);
    QCOMPARE(ExtensionManager::classifyHealth(
                 true, none, none, PV::Dead),
             EH::NotSupported);
    QCOMPARE(ExtensionManager::classifyHealth(
                 true, {QStringLiteral("notifications")}, none,
                 PV::Responsive),
             EH::Limited);
    // Dead beats Limited — a crashed worker means nothing works.
    QCOMPARE(ExtensionManager::classifyHealth(
                 true, {QStringLiteral("notifications")}, none,
                 PV::Dead),
             EH::NotSupported);
    // EXT21: a required-but-unserved core API means the extension's
    // declared purpose can never run — NotSupported even with a
    // live, responsive background (the popup opens, the product
    // function is dead).
    QCOMPARE(ExtensionManager::classifyHealth(
                 true, {QStringLiteral("scripting")},
                 {QStringLiteral("scripting")}, PV::Responsive),
             EH::NotSupported);
    // ...and beats Limited the other way too.
    QCOMPARE(ExtensionManager::classifyHealth(
                 true, {QStringLiteral("scripting"),
                        QStringLiteral("notifications")},
                 {QStringLiteral("scripting")}, PV::Responsive),
             EH::NotSupported);

    QString reason;
    ExtensionManager::classifyHealth(
        true, {QStringLiteral("notifications"),
               QStringLiteral("tabs")}, none, PV::Dead, &reason);
    QVERIFY(reason.contains(QLatin1String("background")));
    QVERIFY(reason.contains(QLatin1String("notifications")));
    reason.clear();
    ExtensionManager::classifyHealth(
        true, {QStringLiteral("cookies")}, none, PV::Responsive,
        &reason);
    QVERIFY(reason.contains(QLatin1String("cookies")));
    reason.clear();
    ExtensionManager::classifyHealth(
        true, none, none, PV::Dead, &reason);
    QVERIFY(reason.contains(QLatin1String("background")));
    // EXT21: the fatal reason names the unserved API and states the
    // extension cannot work — no softening into "limited".
    reason.clear();
    ExtensionManager::classifyHealth(
        true, {QStringLiteral("scripting")},
        {QStringLiteral("scripting")}, PV::Responsive, &reason);
    QVERIFY(reason.contains(QLatin1String("scripting")));
    QVERIFY(reason.contains(QLatin1String("cannot work")));
}

// EXT12: the real measurement — a live service worker must classify
// Healthy and an eval-crashing one NotSupported, driven through the
// same hidden-page probe the app runs.
void tst_ExtensionManager::backgroundHealthProbe()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");

    const QString root = QDir::temp().filePath(
        QStringLiteral("tst-exthealth-%1")
            .arg(QCoreApplication::applicationPid()));
    // Live: only an onInstalled listener — the probe ping errors with
    // "receiving end does not exist" but the worker context registers,
    // which is the healthy-no-listener shape.
    const QString liveDir = writeExtension(
        root, QStringLiteral("tst-health-live"),
        QStringLiteral("1.0.0"));
    // Dead: same manifest shape but the worker throws at eval —
    // Bitwarden's failure mode (Chromium status 15).
    const QString deadDir = writeExtension(
        root, QStringLiteral("tst-health-dead"),
        QStringLiteral("1.0.0"), QString(),
        {QStringLiteral("notifications")});
    QVERIFY(!liveDir.isEmpty() && !deadDir.isEmpty());
    {
        QFile dead(deadDir + QLatin1String("/sw.js"));
        QVERIFY(dead.open(QIODevice::WriteOnly | QIODevice::Truncate));
        dead.write("throw new Error('eval crash');\n");
    }

    ExtensionManager *manager = ExtensionManager::instance();
    const auto infoFor = [manager](const QString &extId) {
        const QList<ExtensionManager::ExtensionInfo> list =
            manager->extensions();
        for (const ExtensionManager::ExtensionInfo &info : list)
            if (info.id == extId)
                return info;
        return ExtensionManager::ExtensionInfo();
    };

    const QString liveId = installFixture(liveDir);
    const QString deadId = installFixture(deadDir);
    QVERIFY2(!liveId.isEmpty() && !deadId.isEmpty(),
             qPrintable(QStringLiteral(
                 "fixtures must install: live=%1 dead=%2")
                 .arg(liveId, deadId)));

    manager->setExtensionEnabled(liveId, true);
    manager->setExtensionEnabled(deadId, true);
    QTRY_VERIFY_WITH_TIMEOUT(
        infoFor(liveId).enabled && infoFor(deadId).enabled, 30000);

    // requestHealthProbe queues ahead of the changed()-driven scan —
    // either path lands the same verdict.
    manager->requestHealthProbe(liveId, false);
    manager->requestHealthProbe(deadId, false);

    using EH = ExtensionManager::ExtensionHealth;
    QTRY_VERIFY_WITH_TIMEOUT(
        infoFor(liveId).health == EH::Healthy, 60000);
    QTRY_VERIFY_WITH_TIMEOUT(
        infoFor(deadId).health == EH::NotSupported, 60000);
    const ExtensionManager::ExtensionInfo dead = infoFor(deadId);
    QVERIFY(!dead.healthReason.isEmpty());
    QVERIFY(dead.healthReason.contains(QLatin1String("background")));
    // "notifications" is unsupported in Qt WebEngine — the reason
    // names the missing surface, not just the crash.
    QVERIFY(dead.missingApis.contains(QLatin1String("notifications")));

    manager->removeExtension(liveId);
    manager->removeExtension(deadId);
    QTest::qWait(500);
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
            "JSON.stringify({state:__arktosHost.state,"
            "bridge:__arktosHost.bridgeUp,"
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
        " var area=__arktosShim.storageArea('session');"
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
            "err:self.__probe.err||null,gaps:__arktosShim.gaps})"))
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

    // Relay keys carry the owning extension's id prefix — exactly the
    // "<extId>#<seq>#<ts>" form the shim mints (QUAL03: the host
    // re-verifies the prefix instead of trusting the caller).
    const QString m1 = QStringLiteral("relayext0000000001#1#t");
    const QString m2 = QStringLiteral("relayext0000000002#2#t");
    const QString m3 = QStringLiteral("relayext0000000001#3#t");
    const QString m4 = QStringLiteral("relayext0000000001#4#t");

    // send fans out to the same-extension peer only.
    const int peers = runtime->relaySend(
        &bg, m1,
        QJsonObject{{QStringLiteral("__arktosMsg"),
                     QJsonObject{{QStringLiteral("mid"), m1}}}});
    QCOMPARE(peers, 1);
    QCOMPARE(popupEvents.size(), 1);
    QCOMPARE(otherEvents.size(), 0);
    QCOMPARE(popupEvents.first().at(0).toString(),
             QStringLiteral("__arktos.relay"));
    QCOMPARE(popupEvents.first().at(1).toJsonObject()
             .value(QLatin1String("mid")).toString(), m1);

    // QUAL03: a foreign bridge that learned the mid may not resolve
    // another extension's message — the pending entry survives.
    QVERIFY(!runtime->relayRespond(
        &other, m1, QJsonValue(QStringLiteral("forged"))));
    QCOMPARE(bgEvents.size(), 0);

    // The peer's response routes back to the origin.
    QVERIFY(runtime->relayRespond(
        &popup, m1,
        QJsonValue(QStringLiteral("pong"))));
    QCOMPARE(bgEvents.size(), 1);
    QCOMPARE(bgEvents.first().at(0).toString(),
             QStringLiteral("__arktos.relayResponse"));
    QCOMPARE(bgEvents.first().at(1).toJsonObject()
             .value(QLatin1String("response")).toString(),
             QStringLiteral("pong"));

    // A spent mid no longer routes.
    QVERIFY(!runtime->relayRespond(
        &popup, m1, QJsonValue()));

    // QUAL03: a mid outside the sender's own id space is refused
    // before delivery or pend — no cross-extension keying.
    QCOMPARE(runtime->relaySend(
                 &bg, m2, QJsonObject()), 0);
    QCOMPARE(popupEvents.size(), 1);

    // peers=0 message (no sibling contexts): nothing pending.
    QCOMPARE(runtime->relaySend(
                 &other, m2, QJsonObject()),
             0);
    QVERIFY(!runtime->relayRespond(
        &other, m2, QJsonValue(1)));

    // Empty peer reply resolves to null after all peers answered.
    QCOMPARE(runtime->relaySend(
                 &bg, m3, QJsonObject()), 1);
    QVERIFY(runtime->relayRespond(
        &popup, m3, QJsonValue()));
    QCOMPARE(bgEvents.size(), 2);
    QVERIFY(bgEvents.last().at(1).toJsonObject()
            .value(QLatin1String("response")).isNull());

    // QUAL03: port channels are scoped the same way — a foreign chan
    // string is refused at every relay entry point, so an extension
    // cannot graft an end onto another extension's port.
    const QString foreignChan =
        QStringLiteral("relayext0000000001#p9");
    QCOMPARE(runtime->relayPortConnect(
                 &other, foreignChan, QStringLiteral("pX"),
                 QString(), QString()), 0);
    runtime->relayPortAccept(&other, foreignChan,
                             QStringLiteral("pX"));
    QCOMPARE(runtime->relayPortMessage(
                 &other, foreignChan, QStringLiteral("pX"),
                 QJsonValue(QStringLiteral("x"))), 0);
    QCOMPARE(bgEvents.size(), 2);   // unchanged — nothing leaked

    // A legit same-extension port round-trips end to end.
    const QString chan = QStringLiteral("relayext0000000001#p1");
    QCOMPARE(runtime->relayPortConnect(
                 &bg, chan, QStringLiteral("p1"),
                 QStringLiteral("named"),
                 QStringLiteral("chrome-extension://relayext0000000001/"
                                "popup.html")), 1);
    QCOMPARE(popupEvents.size(), 3);
    QCOMPARE(popupEvents.last().at(0).toString(),
             QStringLiteral("__arktos.portConnect"));
    // EXT18: the delivered sender carries the connecting document's
    // url — Bitwarden's internal-sender gate keys on it.
    QCOMPARE(popupEvents.last().at(1).toJsonObject()
             .value(QLatin1String("sender")).toObject()
             .value(QLatin1String("url")).toString(),
             QStringLiteral("chrome-extension://relayext0000000001/"
                            "popup.html"));
    runtime->relayPortAccept(&popup, chan, QStringLiteral("pA"));
    QCOMPARE(runtime->relayPortMessage(
                 &bg, chan, QStringLiteral("p1"),
                 QJsonValue(QStringLiteral("hello"))), 1);
    QCOMPARE(popupEvents.size(), 4);
    QCOMPARE(popupEvents.last().at(0).toString(),
             QStringLiteral("__arktos.portMessage"));
    QCOMPARE(popupEvents.last().at(1).toJsonObject()
             .value(QLatin1String("msg")).toString(),
             QStringLiteral("hello"));

    // ...and a foreign bridge cannot tear the channel down either.
    runtime->relayPortDisconnect(&other, foreignChan,
                                 QStringLiteral("pX"));
    QCOMPARE(runtime->relayPortMessage(
                 &popup, chan, QStringLiteral("pA"),
                 QJsonValue(QStringLiteral("back"))), 1);

    // Teardown unregisters; a further send finds no peers.
    runtime->unregisterRelayContext(&popup);
    QCOMPARE(runtime->relaySend(
                 &bg, m4, QJsonObject()), 0);
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
        "['onBeforeNavigate','onCommitted','onDOMContentLoaded',\n"
        " 'onCompleted','onErrorOccurred'].forEach(function(e){\n"
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
    // ER04 — the originless data: document runs no page-level
    // probe, so onDOMContentLoaded arrives via the hub's
    // completion-time fallback — present, and ordered between
    // onCommitted and onCompleted.
    QTRY_VERIFY_WITH_TIMEOUT(
        sawEvent(runtime->host(idA)->page(),
                 QStringLiteral("webNavigation.onDOMContentLoaded")),
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

    // ER03: Chrome parity for onActivated — ONE activeInfo object
    // {tabId, windowId}, never two scalar args — and the scripted
    // ops reach the fixture in causal order: tab2 (the newTab)
    // onCreated before its onRemoved, tab0's setCurrentIndex(0)
    // activation after tab2's creation, and the navigated load's
    // onUpdated 'loading' before 'complete'.
    {
        const QJsonObject order = QJsonDocument::fromJson(
            eval(runtime->host(idA)->page(), QStringLiteral(
                "(function(){var evs=self.__extEvents||[];"
                "function ix(p){for(var i=0;i<evs.length;++i)"
                " if(p(evs[i]))return i;return -1;}"
                "var c0=ix(function(e){return e.n==='tabs.onCreated'"
                "&&e.a[0]&&e.a[0].index===0;});"
                "var id0=c0<0?-1:evs[c0].a[0].id;"
                "var c2=ix(function(e){return e.n==='tabs.onCreated'"
                "&&e.a[0]&&e.a[0].index===1;});"
                "var id2=c2<0?-1:evs[c2].a[0].id;"
                "var a0=ix(function(e){return e.n==='tabs.onActivated'"
                "&&e.a[0]&&e.a[0].tabId===id0;});"
                "var r2=ix(function(e){return e.n==='tabs.onRemoved'"
                "&&e.a[0]===id2;});"
                "var l0=ix(function(e){return e.n==='tabs.onUpdated'"
                "&&e.a[0]===id0&&e.a[1]&&e.a[1].status==='loading';});"
                "var f0=ix(function(e){return e.n==='tabs.onUpdated'"
                "&&e.a[0]===id0&&e.a[1]&&e.a[1].status==='complete';});"
                "var shape=evs.every(function(e){"
                " return e.n!=='tabs.onActivated'"
                "  ||(e.a.length===1&&e.a[0]"
                "   &&typeof e.a[0].tabId==='number'"
                "   &&typeof e.a[0].windowId==='number');});"
                "return JSON.stringify({c2:c2,a0:a0,r2:r2,l0:l0,"
                "f0:f0,shape:shape});})()"))
            .toString().toUtf8()).object();
        QVERIFY2(order.value(QLatin1String("shape")).toBool(),
                 "tabs.onActivated(activeInfo) single-object shape");
        QVERIFY2(order.value(QLatin1String("c2")).toInt(-1) >= 0,
                 "newTab onCreated recorded");
        QVERIFY2(order.value(QLatin1String("r2")).toInt(-1)
                     > order.value(QLatin1String("c2")).toInt(-1),
                 "onRemoved arrives after its tab's onCreated");
        QVERIFY2(order.value(QLatin1String("a0")).toInt(-1)
                     > order.value(QLatin1String("c2")).toInt(-1),
                 "setCurrentIndex activation follows the create");
        QVERIFY2(order.value(QLatin1String("l0")).toInt(-1) >= 0
                     && order.value(QLatin1String("f0")).toInt(-1)
                            > order.value(QLatin1String("l0")).toInt(-1),
                 "onUpdated loading precedes complete");
    }

    // ER04 — webNavigation causal order on the data: navigation:
    // onCommitted -> onDOMContentLoaded -> onCompleted (details.url
    // is the committed url on all three).
    {
        const QJsonObject navOrder = QJsonDocument::fromJson(
            eval(runtime->host(idA)->page(), QStringLiteral(
                "(function(){var evs=self.__extEvents||[];"
                "function ix(n){for(var i=0;i<evs.length;++i)"
                " if(evs[i].n===n&&evs[i].a[0]&&evs[i].a[0].url"
                "  &&evs[i].a[0].url.indexOf('data:text/html')===0)"
                "  return i;return -1;}"
                "return JSON.stringify({"
                "c:ix('webNavigation.onCommitted'),"
                "d:ix('webNavigation.onDOMContentLoaded'),"
                "f:ix('webNavigation.onCompleted')});})()"))
            .toString().toUtf8()).object();
        QVERIFY2(navOrder.value(QLatin1String("c")).toInt(-1) >= 0
                     && navOrder.value(QLatin1String("d")).toInt(-1)
                            > navOrder.value(QLatin1String("c"))
                                  .toInt(-1)
                     && navOrder.value(QLatin1String("f")).toInt(-1)
                            > navOrder.value(QLatin1String("d"))
                                  .toInt(-1),
                 qPrintable(QStringLiteral(
                     "webNavigation committed < domContentLoaded "
                     "< completed — nav events: %1")
                     .arg(eval(runtime->host(idA)->page(),
                          QStringLiteral(
                              "JSON.stringify((self.__extEvents||[])"
                              ".filter(function(e){return e.n"
                              ".indexOf('webNavigation.')===0;}))"))
                          .toString())));
    }

    // ER04 — a real-origin navigation exercises the page-side
    // DOMContentLoaded probe itself (page-level scripts DO inject on
    // http:); the hub's probe-hit counter separates the real signal
    // from the completion fallback above.
    QTcpServer navServer;
    QVERIFY(navServer.listen(QHostAddress::LocalHost));
    const QString navBase = QStringLiteral("http://127.0.0.1:%1")
        .arg(navServer.serverPort());
    QObject::connect(&navServer, &QTcpServer::newConnection, this,
                     [&]() {
        QTcpSocket *socket = navServer.nextPendingConnection();
        QObject::connect(socket, &QTcpSocket::readyRead, socket,
                         [socket]() {
            const QByteArray req = socket->readAll();
            // The page pulls a slow subresource so loadFinished
            // lands well after DOMContentLoaded — the probe's
            // channel hop gets a comfortable window.
            const QByteArray body = req.startsWith("GET /slow")
                ? QByteArray("\x89PNG\r\n\x1a\n")
                : QByteArray("<html><body>nav"
                             "<img src=\"/slow\">"
                             "</body></html>");
            const QByteArray reply =
                "HTTP/1.1 200\r\nContent-Length: "
                + QByteArray::number(body.size())
                + "\r\nConnection: close\r\n\r\n" + body;
            if (req.startsWith("GET /slow"))
                QTimer::singleShot(400, socket,
                                   [socket, reply]() {
                    socket->write(reply);
                    socket->disconnectFromHost();
                });
            else {
                socket->write(reply);
                socket->disconnectFromHost();
            }
        });
    });
    const int probesBefore = hub->domReadyProbeHits();
    view->load(QUrl(navBase + QStringLiteral("/nav")));
    QTRY_VERIFY_WITH_TIMEOUT(hub->domReadyProbeHits() > probesBefore,
                             30000);
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(runtime->host(idA)->page(), QStringLiteral(
            "(function(){var evs=self.__extEvents||[];"
            "function ix(n){for(var i=0;i<evs.length;++i)"
            " if(evs[i].n===n&&evs[i].a[0]&&evs[i].a[0].url==='%1/nav')"
            "  return i;return -1;}"
            "var c=ix('webNavigation.onCommitted'),"
            "d=ix('webNavigation.onDOMContentLoaded'),"
            "f=ix('webNavigation.onCompleted');"
            "return c>=0&&d>c&&f>d;})()")
                .arg(navBase)).toBool(),
        30000);

    // ER04 — a refused connection must emit onErrorOccurred, never
    // silence: bind a port, drop the listener, then navigate at it.
    quint16 deadPort = 0;
    {
        QTcpServer dead;
        QVERIFY(dead.listen(QHostAddress::LocalHost));
        deadPort = dead.serverPort();
    }
    const QString deadUrl = QStringLiteral("http://127.0.0.1:%1/")
        .arg(deadPort);
    view->load(QUrl(deadUrl));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(runtime->host(idA)->page(), QStringLiteral(
            "(self.__extEvents||[]).some(function(e){"
            " return e.n==='webNavigation.onErrorOccurred'&&e.a[0]"
            "  &&e.a[0].url==='%1'"
            "  &&typeof e.a[0].error==='string'"
            "  &&e.a[0].error.indexOf('net::ERR_')===0;})")
                .arg(deadUrl)).toBool(),
        30000);
    // The failure's error page is a browser-internal generated commit
    // (WebPage::setHtml, counted by generatedPageCommitPending) — no
    // webNavigation events may describe it, and the tab's url stays
    // the failed one.  Settle the cycle with a real navigation, then
    // assert nothing webNavigation-shaped was emitted for this tab
    // between the failure and that next navigation's onBeforeNavigate.
    view->load(QUrl(navBase + QStringLiteral("/done")));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(runtime->host(idA)->page(), QStringLiteral(
            "(self.__extEvents||[]).some(function(e){"
            " return e.n==='webNavigation.onCompleted'&&e.a[0]"
            "  &&e.a[0].url==='%1/done';})")
                .arg(navBase)).toBool(),
        30000);
    const QString deadLeak = eval(runtime->host(idA)->page(),
        QStringLiteral(
            "(function(){var evs=self.__extEvents||[];"
            "var ei=-1,bi=-1,tid=-1;"
            "for(var i=0;i<evs.length;++i){var e=evs[i];"
            " if(e.n.indexOf('webNavigation.')!==0)continue;"
            " var a=e.a[0]||{};"
            " if(e.n==='webNavigation.onErrorOccurred'"
            "    &&a.url==='%1'){ei=i;tid=a.tabId;}"
            " if(e.n==='webNavigation.onBeforeNavigate'"
            "    &&a.url==='%2/done')bi=i;}"
            "if(ei<0||bi<0)return 'missing-boundary:'+ei+','+bi;"
            "for(var j=ei+1;j<bi;++j){var x=evs[j];"
            " if(x.n.indexOf('webNavigation.')===0&&x.a[0]"
            "    &&x.a[0].tabId===tid)"
            "  return 'leak:'+x.n+':'+(x.a[0].url||'');}"
            "return '';})()")
            .arg(deadUrl).arg(navBase)).toString();
    QVERIFY2(deadLeak.isEmpty(),
             qPrintable(QStringLiteral(
                 "internal error-page commit leaked: %1 — nav "
                 "events: %2")
                 .arg(deadLeak)
                 .arg(eval(runtime->host(idA)->page(), QStringLiteral(
                     "JSON.stringify((self.__extEvents||[]).filter("
                     "function(e){return e.n.indexOf('webNavigation.')"
                     "===0;}))")).toString())));

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

    // QUAL03: chrome.tabs REPLIES obey the same permission — B's
    // query returns the tabs but without url/title, A's 'tabs'
    // permission keeps them visible.
    ExtensionShimBackend *backendB =
        runtime->host(idB)->bridge()->backend();
    QVERIFY(backendB);
    const QJsonObject queryArgs{
        {QStringLiteral("query"), QJsonObject()}};
    const QJsonArray bTabs = backendB->invoke(
        QStringLiteral("tabs"), QStringLiteral("query"), queryArgs)
        .value(QLatin1String("result")).toArray();
    QVERIFY(!bTabs.isEmpty());
    for (const QJsonValue &v : bTabs) {
        QVERIFY(!v.toObject().contains(QLatin1String("url")));
        QVERIFY(!v.toObject().contains(QLatin1String("title")));
    }
    ExtensionShimBackend *backendA0 =
        runtime->host(idA)->bridge()->backend();
    QVERIFY(backendA0);
    const QJsonArray aTabs = backendA0->invoke(
        QStringLiteral("tabs"), QStringLiteral("query"), queryArgs)
        .value(QLatin1String("result")).toArray();
    QVERIFY(!aTabs.isEmpty());
    QVERIFY(aTabs.first().toObject().contains(QLatin1String("url")));

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

// ER11 — observe-only chrome.webRequest.  Fixture A declares
// 'webRequest' + loopback host coverage and subscribes to every
// servable event (with requestHeaders/responseHeaders asks), a
// listener whose url filter never matches, a 'blocking' listener
// (typed unsupported, observe-only degrade) and the unservable
// onAuthRequired.  A real page load + scripted fetch over a
// loopback server exercises the interceptor ledger and the
// resource-timing completion probe; a refused-port navigation
// exercises onErrorOccurred.  Fixture B holds 'webRequest' but no
// covering host permission — nothing may reach it.  Fixture C
// lacks the permission — registration itself is the typed denial.
void tst_ExtensionManager::extensionWebRequest()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");

    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    const QString base = QStringLiteral("http://127.0.0.1:%1")
        .arg(server.serverPort());
    const QByteArray pageBody =
        "<html><head><script>addEventListener('DOMContentLoaded',"
        "function(){fetch('/data.json').then(function(r){"
        "return r.text();}).then(function(t){document.title="
        "'fetched:'+t;});});</script></head><body>wr</body></html>";
    const QByteArray jsonBody = "{\"ok\":true}";
    QObject::connect(&server, &QTcpServer::newConnection, this,
                     [&]() {
        QTcpSocket *socket = server.nextPendingConnection();
        QObject::connect(socket, &QTcpSocket::readyRead, socket,
                         [socket, pageBody, jsonBody]() {
            const QByteArray request = socket->readAll();
            const QByteArray body =
                request.startsWith("GET /data.json")
                    ? jsonBody : pageBody;
            socket->write("HTTP/1.1 200\r\nContent-Length: "
                + QByteArray::number(body.size())
                + "\r\nConnection: close\r\n\r\n" + body);
            socket->disconnectFromHost();
        });
    });

    const QString recorder = QStringLiteral(
        "self.__wr=[];\n"
        "self.__wrRec=function(n){return function(d){\n"
        " self.__wr.push({n:n,url:d.url,type:d.type,tabId:d.tabId,\n"
        "  requestId:d.requestId,method:d.method,\n"
        "  reqHeaders:!!d.requestHeaders,\n"
        "  respHeaders:!!d.responseHeaders,\n"
        "  error:d.error||null});};};\n");
    const QString swA = recorder + QStringLiteral(
        "var urlsAll={urls:['<all_urls>']};\n"
        "var urlsLoop={urls:['http://127.0.0.1:*/*'],\n"
        " types:['main_frame','xmlhttprequest']};\n"
        "chrome.webRequest.onBeforeRequest.addListener(\n"
        " self.__wrRec('onBeforeRequest'),urlsAll);\n"
        "chrome.webRequest.onBeforeSendHeaders.addListener(\n"
        " self.__wrRec('onBeforeSendHeaders'),urlsAll,\n"
        " ['requestHeaders']);\n"
        "chrome.webRequest.onCompleted.addListener(\n"
        " self.__wrRec('onCompleted'),urlsLoop,['responseHeaders']);\n"
        "chrome.webRequest.onErrorOccurred.addListener(\n"
        " self.__wrRec('onErrorOccurred'),urlsAll);\n"
        "chrome.webRequest.onCompleted.addListener(\n"
        " self.__wrRec('onCompletedFiltered'),\n"
        " {urls:['http://unrelated.example/*']});\n"
        "chrome.webRequest.onBeforeRequest.addListener(\n"
        " self.__wrRec('onBeforeRequestBlocking'),urlsAll,\n"
        " ['blocking']);\n"
        "chrome.webRequest.onAuthRequired.addListener(\n"
        " self.__wrRec('onAuthRequired'),urlsAll);\n"
        "chrome.webRequest.handlerBehaviorChanged();\n");
    const QString swB = recorder + QStringLiteral(
        "chrome.webRequest.onBeforeRequest.addListener(\n"
        " self.__wrRec('onBeforeRequest'),{urls:['<all_urls>']});\n"
        "chrome.webRequest.onCompleted.addListener(\n"
        " self.__wrRec('onCompleted'),{urls:['<all_urls>']});\n");
    const QString swC = recorder + QStringLiteral(
        "chrome.webRequest.onBeforeRequest.addListener(\n"
        " self.__wrRec('onBeforeRequest'),{urls:['<all_urls>']});\n");

    struct Fixture {
        QString name;
        QString dir;
        QString permissions;
        QString hostPermissions;
        QString sw;
    };
    const QList<Fixture> fixtures = {
        {QStringLiteral("tst-webreq-a"), QString(),
         QStringLiteral("\"storage\",\"webRequest\""),
         QStringLiteral("\"http://127.0.0.1:*/*\""), swA},
        {QStringLiteral("tst-webreq-b"), QString(),
         QStringLiteral("\"webRequest\""),
         QStringLiteral("\"https://allowed.example/*\""), swB},
        {QStringLiteral("tst-webreq-c"), QString(),
         QStringLiteral("\"storage\""),
         QStringLiteral("\"http://127.0.0.1:*/*\""), swC},
    };
    const QString root = QDir::temp().filePath(
        QStringLiteral("tst-extwr-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(root));
    QList<Fixture> written;
    for (Fixture f : fixtures) {
        f.dir = root + QLatin1Char('/') + f.name;
        QVERIFY(QDir().mkpath(f.dir));
        QFile manifestFile(f.dir + QLatin1String("/manifest.json"));
        QVERIFY(manifestFile.open(QIODevice::WriteOnly));
        manifestFile.write("{\"manifest_version\":3,\"name\":\""
            + f.name.toUtf8() + "\",\"version\":\"1.0\",\"permissions\":["
            + f.permissions.toUtf8() + "],\"host_permissions\":["
            + f.hostPermissions.toUtf8()
            + "],\"background\":{\"service_worker\":\"sw.js\"}}");
        manifestFile.close();
        QFile worker(f.dir + QLatin1String("/sw.js"));
        QVERIFY(worker.open(QIODevice::WriteOnly));
        worker.write(f.sw.toUtf8());
        worker.close();
        written.append(f);
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
    const QString idA = loadReady(written.at(0).dir, written.at(0).name);
    const QString idB = loadReady(written.at(1).dir, written.at(1).name);
    const QString idC = loadReady(written.at(2).dir, written.at(2).name);
    QVERIFY2(!idA.isEmpty() && !idB.isEmpty() && !idC.isEmpty(),
             "fixtures loaded + hosted");

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

    // The subscribe calls ride the bridge — wait for the watcher's
    // registry before driving traffic (A keeps 6 registrations:
    // the four servable events, the filtered + blocking listeners —
    // onAuthRequired rejects without registering).
    QTRY_VERIFY_WITH_TIMEOUT(
        ExtensionRequestWatcher::instance()->registrationCount(idA)
            == 6
            && ExtensionRequestWatcher::instance()
                       ->registrationCount(idB) == 2,
        30000);

    hub->setTracking(true);
    hub->syncNow();
    BrowserMainWindow *window = new BrowserMainWindow;
    TabWidget *tw = window->tabWidget();
    QVERIFY(tw && tw->count() >= 1);
    hub->syncNow();
    WebView *view = tw->currentWebView();
    QVERIFY(view);

    view->load(QUrl(base + QStringLiteral("/page")));
    const auto saw = [&eval](QWebEnginePage *page,
                             const QString &predicate) {
        return eval(page, QStringLiteral(
            "(self.__wr||[]).some(%1)").arg(predicate)).toBool();
    };

    // onBeforeRequest for the navigation, onCompleted for both the
    // main-frame document and the scripted fetch (the latter via the
    // resource-timing probe — allow the flush interval headroom).
    const auto dumpEvents = [&eval](QWebEnginePage *page) {
        return eval(page, QStringLiteral(
            "JSON.stringify(self.__wr||[])")).toString();
    };
    QElapsedTimer waitBefore;
    waitBefore.start();
    while (waitBefore.elapsed() < 30000
           && !saw(runtime->host(idA)->page(), QStringLiteral(
               "function(e){return e.n==='onBeforeRequest'"
               "&&e.url==='%1/page'&&e.type==='main_frame'"
               "&&e.method==='GET'&&e.tabId>=0&&e.requestId;}")
                   .arg(base)))
        QTest::qWait(100);
    QVERIFY2(saw(runtime->host(idA)->page(), QStringLiteral(
            "function(e){return e.n==='onBeforeRequest'"
            "&&e.url==='%1/page'&&e.type==='main_frame'"
            "&&e.method==='GET'&&e.tabId>=0&&e.requestId;}")
                .arg(base)),
             qPrintable(QStringLiteral(
                 "onBeforeRequest for the navigation — events: %1")
                 .arg(dumpEvents(runtime->host(idA)->page()))));
    QTRY_VERIFY_WITH_TIMEOUT(
        saw(runtime->host(idA)->page(), QStringLiteral(
            "function(e){return e.n==='onCompleted'"
            "&&e.url==='%1/data.json'&&e.type==='xmlhttprequest';}")
                .arg(base)), 30000);

    // Details shape: the main-frame onCompleted carries the real
    // responseHeaders (asked via extraInfoSpec); the send-headers
    // event carried requestHeaders; before/completed pair on one
    // requestId.
    const QJsonObject shape = QJsonDocument::fromJson(
        eval(runtime->host(idA)->page(), QStringLiteral(
            "(function(){var e=self.__wr||[];"
            "function any(p){return e.some(p);}"
            "function rid(u){for(var i=0;i<e.length;++i)"
            " if(e[i].n==='onBeforeRequest'&&e[i].url===u)"
            "  return e[i].requestId;return null;}"
            "function crid(u){for(var i=0;i<e.length;++i)"
            " if(e[i].n==='onCompleted'&&e[i].url===u)"
            "  return e[i].requestId;return null;}"
            "return JSON.stringify({"
            "ev:e.map(function(x){return x.n+'|'+x.type+'|'+x.url"
            "+(x.respHeaders?'|R':'')+(x.reqHeaders?'|Q':'')+'|'"
            "+x.error;}).join(';'),"
            "resp:any(function(x){return x.n==='onCompleted'"
            " &&x.url==='%1/page'&&x.type==='main_frame'"
            " &&x.respHeaders;}),"
            "req:any(function(x){return x.n==='onBeforeSendHeaders'"
            " &&x.url==='%1/page'&&x.reqHeaders;}),"
            "pair:rid('%1/page')&&rid('%1/page')===crid('%1/page'),"
            "blockingSaw:any(function(x){"
            " return x.n==='onBeforeRequestBlocking'"
            " &&x.url==='%1/page';}),"
            "filteredOut:!any(function(x){"
            " return x.n==='onCompletedFiltered';})"
            "});})()").arg(base)).toString().toUtf8()).object();
    QVERIFY2(shape.value(QLatin1String("resp")).toBool(),
             qPrintable(QStringLiteral(
                 "onCompleted main-frame responseHeaders served — "
                 "events: %1")
                 .arg(shape.value(QLatin1String("ev")).toString())));
    QVERIFY2(shape.value(QLatin1String("req")).toBool(),
             "onBeforeSendHeaders requestHeaders served");
    QVERIFY2(shape.value(QLatin1String("pair")).toBool(),
             "before/completed share one requestId");
    QVERIFY2(shape.value(QLatin1String("blockingSaw")).toBool(),
             "blocking listener still observes (MV3 degrade)");
    QVERIFY2(shape.value(QLatin1String("filteredOut")).toBool(),
             "non-matching urls filter stays silent");

    // The typed degrades: 'blocking' and onAuthRequired each answer
    // 'unsupported' — observable in the host's gap list, never
    // silently dropped.
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime->host(idA)->shimGaps().contains(
            QStringLiteral("chrome.webRequest.subscribe: unsupported")),
        20000);
    // C never got the permission — its subscribe is the typed
    // denial, also health-visible.
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime->host(idC)->shimGaps().contains(QStringLiteral(
            "chrome.webRequest.subscribe: no-webRequest-permission")),
        20000);

    // onErrorOccurred — a refused connection must report a net::
    // error, never silence.
    quint16 deadPort = 0;
    {
        QTcpServer dead;
        QVERIFY(dead.listen(QHostAddress::LocalHost));
        deadPort = dead.serverPort();
    }
    const QString deadUrl = QStringLiteral("http://127.0.0.1:%1/")
        .arg(deadPort);
    view->load(QUrl(deadUrl));
    QTRY_VERIFY_WITH_TIMEOUT(
        saw(runtime->host(idA)->page(), QStringLiteral(
            "function(e){return e.n==='onErrorOccurred'"
            "&&e.url==='%1'&&e.error"
            "&&e.error.indexOf('net::ERR_')===0;}")
                .arg(deadUrl)), 30000);

    // Host-permission scoping — B's listeners registered fine but no
    // loopback URL is covered by https://allowed.example/*, so
    // nothing reached it.
    QCOMPARE(eval(runtime->host(idB)->page(), QStringLiteral(
        "(self.__wr||[]).some(function(e){"
        " return String(e.url||'').indexOf('127.0.0.1')>=0;})"))
            .toBool(), false);

    // Cleanup.
    window->close();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    manager->setExtensionEnabled(idA, false);
    manager->setExtensionEnabled(idB, false);
    manager->setExtensionEnabled(idC, false);
    QTRY_VERIFY_WITH_TIMEOUT(
        !runtime->host(idA) && !runtime->host(idB)
            && !runtime->host(idC), 20000);
    hub->setTracking(false);
    manager->removeExtension(idA);
    manager->removeExtension(idB);
    manager->removeExtension(idC);
    QCOMPARE(ExtensionRequestWatcher::instance()
                 ->registrationCount(idA), 0);
    QDir(root).removeRecursively();
}

// ER05 — the runtime lifecycle events + the window-level mirror.
// A fixture extension records runtime.onInstalled/onStartup and
// windows.onCreated/onRemoved/onFocusChanged; the leg drives the
// whole reason matrix: fresh install answers {reason:'install'}
// once and NO onStartup; a relaunch (disable + the runtime's
// restartLifecycleSession seam + re-enable) answers onStartup with
// no second onInstalled; a version bump answers
// {reason:'update',previousVersion}; an app-version change answers
// {reason:'browser_update'}; and a scripted window open/close lands
// the windows.* events.
void tst_ExtensionManager::extensionRuntimeLifecycle()
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
        "chrome.runtime.onInstalled.addListener(\n"
        " self.__extRec('runtime.onInstalled'));\n"
        "chrome.runtime.onStartup.addListener(\n"
        " self.__extRec('runtime.onStartup'));\n"
        "['onCreated','onRemoved','onFocusChanged'].forEach(\n"
        " function(e){chrome.windows[e].addListener(\n"
        "  self.__extRec('windows.'+e));});\n";

    const QString dir = QDir::temp().filePath(
        QStringLiteral("tst-extlc-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dir));
    const auto writeFixture = [&dir, &swRecord](
            const QString &version) {
        QFile f(dir + QLatin1String("/manifest.json"));
        if (!f.open(QIODevice::WriteOnly))
            return false;
        f.write("{\"manifest_version\":3,\"name\":\"tst-lifecycle\","
                "\"version\":\"" + version.toUtf8()
                + "\",\"permissions\":[\"storage\"],"
                  "\"background\":{\"service_worker\":\"sw.js\"}}");
        f.close();
        QFile w(dir + QLatin1String("/sw.js"));
        if (!w.open(QIODevice::WriteOnly))
            return false;
        w.write(swRecord.toUtf8());
        w.close();
        return true;
    };
    QVERIFY(writeFixture(QStringLiteral("1.0")));

    ExtensionManager *manager = ExtensionManager::instance();
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();
    ExtensionEventHub *hub = ExtensionEventHub::instance();

    auto loaded = std::make_shared<QString>();
    const auto watch = [loaded](
            const ExtensionManager::ExtensionInfo &info) {
        if (info.name.startsWith(QLatin1String("tst-lifecycle"))
                && info.error.isEmpty())
            *loaded = info.id;
    };
    QObject::connect(manager, &ExtensionManager::extensionLoaded,
                     this, watch);
    QObject::connect(manager, &ExtensionManager::extensionInstalled,
                     this, watch);
    manager->grantConsent(dir);
    manager->loadExtension(dir);
    QElapsedTimer loadTimer;
    loadTimer.start();
    while (loaded->isEmpty() && loadTimer.elapsed() < 30000)
        QTest::qWait(50);
    const QString id = *loaded;
    QVERIFY2(!id.isEmpty(), "lifecycle fixture loaded");
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
    auto count = [&eval](QWebEnginePage *page, const QString &name) {
        return eval(page, QStringLiteral(
            "(self.__extEvents||[]).filter(function(e){return e.n===\"")
            + name + QLatin1String("\";}).length")).toInt();
    };
    const auto hostPage = [runtime, &id]() {
        ExtensionBackgroundHost *h = runtime->host(id);
        return h ? h->page() : nullptr;
    };
    const auto respawn = [this, manager, runtime, &id]() {
        manager->setExtensionEnabled(id, false);
        QTRY_VERIFY_WITH_TIMEOUT(!runtime->host(id), 20000);
        manager->setExtensionEnabled(id, true);
        QTRY_VERIFY_WITH_TIMEOUT(
            runtime->host(id)
                && runtime->host(id)->state()
                   == ExtensionBackgroundHost::State::Ready,
            45000);
    };

    // --- fresh install: onInstalled{install} once, no onStartup ----
    QTRY_VERIFY_WITH_TIMEOUT(
        count(hostPage(), QStringLiteral("runtime.onInstalled")) == 1,
        20000);
    QCOMPARE(eval(hostPage(), QStringLiteral(
        "(self.__extEvents||[]).some(function(e){"
        " return e.n==='runtime.onInstalled'&&e.a[0]"
        "   &&e.a[0].reason==='install'"
        "   &&e.a[0].previousVersion===undefined;})")).toBool(),
        true);
    // Chrome does not fire onStartup on the installing session.
    QTest::qWait(1500);
    QCOMPARE(count(hostPage(), QStringLiteral("runtime.onStartup")),
             0);

    // --- windows.* on open/focus/close -----------------------------
    hub->setTracking(true);
    hub->syncNow();   // silent baseline — the fixture's first host
                      // already exists; window tracking reseeds.
    BrowserMainWindow *window = new BrowserMainWindow;
    hub->syncNow();
    QTRY_VERIFY_WITH_TIMEOUT(
        count(hostPage(), QStringLiteral("windows.onCreated")) >= 1,
        20000);
    // The onCreated arg is Chrome's Window object — an id-carrying
    // object, never an undefined arg list.
    QCOMPARE(eval(hostPage(), QStringLiteral(
        "(self.__extEvents||[]).some(function(e){"
        " return e.n==='windows.onCreated'&&e.a.length===1"
        "   &&e.a[0]&&typeof e.a[0].id==='number';})")).toBool(),
        true);
    window->show();
    window->activateWindow();
    hub->syncNow();
    QTRY_VERIFY_WITH_TIMEOUT(
        count(hostPage(), QStringLiteral("windows.onFocusChanged"))
            >= 1,
        20000);
    window->close();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    hub->syncNow();
    QTRY_VERIFY_WITH_TIMEOUT(
        count(hostPage(), QStringLiteral("windows.onRemoved")) >= 1,
        20000);

    // --- relaunch: onStartup fires, onInstalled does not -----------
    respawn();
    // The respawn was still THIS session — the dedupe held.
    QTest::qWait(500);
    QCOMPARE(count(hostPage(), QStringLiteral("runtime.onStartup")),
             0);
    runtime->restartLifecycleSession();
    respawn();
    QTRY_VERIFY_WITH_TIMEOUT(
        count(hostPage(), QStringLiteral("runtime.onStartup")) == 1,
        20000);
    QTest::qWait(500);
    QCOMPARE(count(hostPage(), QStringLiteral("runtime.onInstalled")),
             0);

    // --- version bump: {reason:'update',previousVersion} -----------
    manager->setExtensionEnabled(id, false);
    QTRY_VERIFY_WITH_TIMEOUT(!runtime->host(id), 20000);
    QVERIFY(writeFixture(QStringLiteral("1.1")));
    manager->setExtensionEnabled(id, true);
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime->host(id)
            && runtime->host(id)->state()
               == ExtensionBackgroundHost::State::Ready,
        45000);
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(hostPage(), QStringLiteral(
            "(self.__extEvents||[]).some(function(e){"
            " return e.n==='runtime.onInstalled'&&e.a[0]"
            "   &&e.a[0].reason==='update'"
            "   &&e.a[0].previousVersion==='1.0';})")).toBool(),
        20000);
    // The update was mid-session — no onStartup for it.
    QTest::qWait(500);
    QCOMPARE(count(hostPage(), QStringLiteral("runtime.onStartup")),
             0);

    // --- app version bump: {reason:'browser_update'} ---------------
    const QString realVersion =
        QCoreApplication::applicationVersion();
    QCoreApplication::setApplicationVersion(
        QStringLiteral("0.0-erlifecycle"));
    respawn();
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(hostPage(), QStringLiteral(
            "(self.__extEvents||[]).some(function(e){"
            " return e.n==='runtime.onInstalled'&&e.a[0]"
            "   &&e.a[0].reason==='browser_update';})")).toBool(),
        20000);
    QCoreApplication::setApplicationVersion(realVersion);
    // Let the record settle on the real version — restoring the app
    // version IS a browser-version change against the persisted
    // record, so this respawn fires a last browser_update and writes
    // {1.1, realVersion}.  The unload/reload below then sees a
    // matching record and fires nothing.
    respawn();

    // --- session unload keeps the record — it is not an uninstall --
    // (Chrome parity for a dev-mode load: a same-session reload is
    // not a fresh install, so neither lifecycle event refires.)
    manager->removeExtension(id);
    QTRY_VERIFY_WITH_TIMEOUT(!runtime->host(id), 20000);
    hub->setTracking(false);

    loaded->clear();
    manager->grantConsent(dir);
    manager->loadExtension(dir);
    QElapsedTimer reloadTimer;
    reloadTimer.start();
    while (loaded->isEmpty() && reloadTimer.elapsed() < 30000)
        QTest::qWait(50);
    const QString idReload = *loaded;
    QVERIFY2(!idReload.isEmpty(), "fixture reloaded after unload");
    manager->setExtensionEnabled(idReload, true);
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime->host(idReload)
            && runtime->host(idReload)->state()
               == ExtensionBackgroundHost::State::Ready,
        45000);
    QTest::qWait(1500);
    QCOMPARE(count(runtime->host(idReload)->page(),
                   QStringLiteral("runtime.onInstalled")), 0);
    QCOMPARE(count(runtime->host(idReload)->page(),
                   QStringLiteral("runtime.onStartup")), 0);
    manager->setExtensionEnabled(idReload, false);
    QTRY_VERIFY_WITH_TIMEOUT(!runtime->host(idReload), 20000);
    manager->removeExtension(idReload);

    // --- an installed extension: uninstall drops the record, a
    // reinstall is a fresh install again ----------------------------
    const QString dirB = QDir::temp().filePath(
        QStringLiteral("tst-extlcB-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dirB));
    {
        QFile f(dirB + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-lifecycle-b\","
                "\"version\":\"1.0\",\"permissions\":[\"storage\"],"
                "\"background\":{\"service_worker\":\"sw.js\"}}");
        f.close();
        QFile w(dirB + QLatin1String("/sw.js"));
        QVERIFY(w.open(QIODevice::WriteOnly));
        w.write(swRecord.toUtf8());
        w.close();
    }
    loaded->clear();
    manager->grantConsent(dirB);
    manager->installExtension(dirB);
    QElapsedTimer installTimer;
    installTimer.start();
    while (loaded->isEmpty() && installTimer.elapsed() < 30000)
        QTest::qWait(50);
    const QString idB = *loaded;
    QVERIFY2(!idB.isEmpty(), "installed fixture landed");
    manager->setExtensionEnabled(idB, true);
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime->host(idB)
            && runtime->host(idB)->state()
               == ExtensionBackgroundHost::State::Ready,
        45000);
    QTRY_VERIFY_WITH_TIMEOUT(
        count(runtime->host(idB)->page(),
              QStringLiteral("runtime.onInstalled")) == 1,
        20000);

    manager->removeExtension(idB);
    QTRY_VERIFY_WITH_TIMEOUT(!runtime->host(idB), 20000);
    // The record died with the uninstall — this reinstall is a
    // fresh install, not a relaunch.
    loaded->clear();
    manager->grantConsent(dirB);
    manager->installExtension(dirB);
    installTimer.restart();
    while (loaded->isEmpty() && installTimer.elapsed() < 30000)
        QTest::qWait(50);
    const QString idB2 = *loaded;
    QVERIFY2(!idB2.isEmpty(), "fixture reinstalled");
    manager->setExtensionEnabled(idB2, true);
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime->host(idB2)
            && runtime->host(idB2)->state()
               == ExtensionBackgroundHost::State::Ready,
        45000);
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(runtime->host(idB2)->page(), QStringLiteral(
            "(self.__extEvents||[]).some(function(e){"
            " return e.n==='runtime.onInstalled'&&e.a[0]"
            "   &&e.a[0].reason==='install';})")).toBool(),
        20000);
    QTest::qWait(1000);
    QCOMPARE(count(runtime->host(idB2)->page(),
                   QStringLiteral("runtime.onStartup")), 0);

    manager->setExtensionEnabled(idB2, false);
    QTRY_VERIFY_WITH_TIMEOUT(!runtime->host(idB2), 20000);
    manager->removeExtension(idB2);
    QDir(dir).removeRecursively();
    QDir(dirB).removeRecursively();
}
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
            "!!(window.__arktosHost&&__arktosHost.bridgeUp"
            "&&window.__arktosShim)")).toBool(),
        30000);
    QCOMPARE(eval(popup, QStringLiteral(
                 "self.__popupBootRan===true")).toBool(),
             true);
    // The page injection skipped the background loader — the
    // document marks itself 'page'-kind.
    QCOMPARE(eval(popup, QStringLiteral(
                 "window.__arktosHost.kind")).toString(),
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
            "!!(window.__arktosHost&&__arktosHost.bridgeUp"
            "&&window.__arktosShim)")).toBool(),
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

// ER01 — the page agent.  A fixture extension declares
// host_permissions + content_scripts on <all_urls>; its background
// echoes sendMessage with the MessageSender it saw, and its declared
// content script calls chrome.runtime.sendMessage from the injected
// world.  The leg proves the whole path end to end: the profile-
// wide agent injection lands in the shared AgentWorldId realm (and
// NOT in the page's main world), the declared content script runs
// through the closure that binds its own chrome, the message routes
// through the page's arktosExt channel object into the hosted
// background, and the response comes back.  The negative legs prove
// the C++ gate: a foreign extension id and an uncovered url both get
// the typed 'no-host-access' absent-API reply.
void tst_ExtensionManager::extensionPageAgent()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");

    const QString dir = QDir::temp().filePath(
        QStringLiteral("tst-extagent-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dir));
    {
        QFile f(dir + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-agent\","
                "\"version\":\"1.0\",\"permissions\":[\"storage\"],"
                "\"host_permissions\":[\"<all_urls>\"],"
                "\"background\":{\"service_worker\":\"sw.js\"},"
                "\"content_scripts\":[{\"matches\":[\"<all_urls>\"],"
                "\"js\":[\"cs.js\"],\"run_at\":\"document_idle\"}]}");
        f.close();
        QFile w(dir + QLatin1String("/sw.js"));
        QVERIFY(w.open(QIODevice::WriteOnly));
        w.write(
            "chrome.runtime.onMessage.addListener("
            "function(m,s,r){"
            " r({echo:m,"
            "   senderId:(s&&s.id)||null,"
            "   senderUrl:(s&&s.url)||null,"
            "   senderOrigin:(s&&s.origin)||null,"
            "   senderTab:!!(s&&s.tab)});"
            " return false; });\n");
        w.close();
        QFile c(dir + QLatin1String("/cs.js"));
        QVERIFY(c.open(QIODevice::WriteOnly));
        c.write(
            "window.__csReply='pending';\n"
            "try{chrome.runtime.sendMessage({from:'cs',k:1},"
            "function(r){"
            "window.__csReply=(r===undefined)"
            " ?{undef:true,"
            "   err:(chrome.runtime.lastError"
            "        &&chrome.runtime.lastError.message)||null}"
            " :r;});}catch(e){window.__csReply={threw:String(e)};}\n");
        c.close();
    }

    ExtensionManager *manager = ExtensionManager::instance();
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();
    auto loaded = std::make_shared<QString>();
    QObject::connect(manager, &ExtensionManager::extensionLoaded,
                     this, [loaded](
                         const ExtensionManager::ExtensionInfo &info) {
        if (info.name == QLatin1String("tst-agent")
                && info.error.isEmpty())
            *loaded = info.id;
    });
    manager->grantConsent(dir);
    manager->loadExtension(dir);
    QTRY_VERIFY_WITH_TIMEOUT(!loaded->isEmpty(), 30000);
    const QString id = *loaded;
    manager->setExtensionEnabled(id, true);
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime->host(id)
            && runtime->host(id)->state()
                   == ExtensionBackgroundHost::State::Ready,
        45000);
    ExtensionBackgroundHost *host = runtime->host(id);
    QVERIFY(host);
    QVERIFY(host->contentScriptMatches()
                .contains(QStringLiteral("<all_urls>")));

    auto eval = [](QWebEnginePage *page, const QString &source,
                   quint32 worldId = 0) {
        auto result = std::make_shared<QVariant>();
        page->runJavaScript(
            source, worldId,
            [result](const QVariant &v) { *result = v; });
        QElapsedTimer t;
        t.start();
        while (!result->isValid() && t.elapsed() < 15000)
            QTest::qWait(50);
        return *result;
    };
    const quint32 agentWorld =
        ExtensionBackgroundRuntime::AgentWorldId;
    // Parenthesised: 'probe + ".field"' must bind to the whole
    // fallback expression, not to the object literal (member access
    // outranks '||', and a returned object QVariant is not toBool).
    const QString probe = QStringLiteral(
        "((window.__arktosExtAgents&&__arktosExtAgents['%1'])"
        "||{active:false,bridgeUp:false})").arg(id);

    // --- a page inside the extension's grants ----------------------
    // Loopback http — a real origin where BOTH script levels land:
    // the profile-level agent script injects everywhere, but
    // WebPage's page-level channel bootstrap (the forwarder the
    // agent talks through) does not run on data:/about: documents —
    // measured with a world probe — so an originless page could
    // never carry the bridge.
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    const QString base = QStringLiteral("http://127.0.0.1:%1")
        .arg(server.serverPort());
    QObject::connect(&server, &QTcpServer::newConnection,
                     this, [&]() {
        QTcpSocket *socket = server.nextPendingConnection();
        QObject::connect(socket, &QTcpSocket::readyRead,
                         socket, [socket]() {
            socket->readAll();
            const QByteArray body =
                "<html><body>agent-page</body></html>";
            socket->write(
                "HTTP/1.1 200\r\nContent-Type: text/html\r\n"
                "Content-Length: "
                + QByteArray::number(body.size())
                + "\r\nConnection: close\r\n\r\n" + body);
            socket->disconnectFromHost();
        });
    });
    auto *view = new QWebEngineView;
    auto *page = new WebPage(
        BrowserApplication::instance()->webEngineProfile(), view);
    view->setPage(page);
    QSignalSpy loadSpy(page, &QWebEnginePage::loadFinished);
    page->load(QUrl(base + QStringLiteral("/agent")));
    QTRY_VERIFY_WITH_TIMEOUT(loadSpy.count() >= 1, 30000);

    // The agent installs + handshakes; the declared content script
    // runs and gets its sendMessage answered by the hosted
    // background.
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(page, probe + QStringLiteral(".active"),
             agentWorld).toBool(),
        30000);
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(page, probe + QStringLiteral(".bridgeUp"),
             agentWorld).toBool(),
        30000);
    {
        bool csDone = false;
        QElapsedTimer csWait;
        csWait.start();
        while (csWait.elapsed() < 30000 && !csDone) {
            csDone = eval(page, QStringLiteral(
                "window.__csReply&&window.__csReply!=='pending'"),
                agentWorld).toBool();
            if (!csDone)
                QTest::qWait(200);
        }
        QVERIFY2(csDone, qPrintable(QStringLiteral(
            "cs did not finish: %1")
            .arg(eval(page, QStringLiteral(
                 "JSON.stringify({reply:(typeof window.__csReply"
                 "==='undefined')?'<unset>':window.__csReply,"
                 "probe:(window.__arktosExtAgents"
                 "&&__arktosExtAgents['%1'])"
                 "?__arktosExtAgents['%1'].probe():null})").arg(id),
                 agentWorld).toString())));
    }
    const QJsonObject reply = QJsonDocument::fromJson(
        eval(page, QStringLiteral("JSON.stringify(window.__csReply)"),
             agentWorld)
            .toString().toUtf8()).object();
    QVERIFY2(!reply.contains(QLatin1String("undef"))
                 && !reply.contains(QLatin1String("threw")),
             qPrintable(QStringLiteral("cs sendMessage failed: %1")
                 .arg(eval(page, QStringLiteral(
                      "JSON.stringify((window"
                      ".__arktosExtAgents&&__arktosExtAgents"
                      "['%1']||{}).probe"
                      "?window.__arktosExtAgents['%1']"
                      ".probe():null)").arg(id),
                      agentWorld).toString())));
    const QJsonObject echo =
        reply.value(QLatin1String("echo")).toObject();
    QCOMPARE(echo.value(QLatin1String("from")).toString(),
             QStringLiteral("cs"));
    QCOMPARE(echo.value(QLatin1String("k")).toInt(), 1);
    // sender.url/origin are the honest page identity — not a
    // chrome-extension: origin (that forgery is exactly what
    // senderIsInternal gates must NOT see) and not whatever a
    // caller passed in args: the bridge reads the committed url.
    QCOMPARE(reply.value(QLatin1String("senderUrl")).toString(),
             base + QStringLiteral("/agent"));
    QCOMPARE(reply.value(QLatin1String("senderOrigin")).toString(),
             base);
    QCOMPARE(reply.value(QLatin1String("senderId")).toString(), id);

    // The page's own world stays clean — our chrome.* lives only in
    // the isolated agent realm.  (window.chrome itself is always an
    // object: Chromium's built-in stub — the honest check is that
    // neither our agent registry nor any runtime surface leaked.)
    QCOMPARE(eval(page, QStringLiteral(
        "typeof window.__arktosExtAgents")).toString(),
             QStringLiteral("undefined"));
    QCOMPARE(eval(page, QStringLiteral(
        "typeof (window.chrome||{}).runtime")).toString(),
             QStringLiteral("undefined"));

    // --- the C++ transport end, driven directly --------------------
    ExtensionPageBridge *bridge =
        page->findChild<ExtensionPageBridge *>();
    QVERIFY2(bridge, "WebPage carries the arktosExt bridge");
    // hello is the scoping handshake: covered page -> ok.
    QCOMPARE(bridge->invoke(
                 QStringLiteral("hello"),
                 QJsonObject{{QStringLiteral("ext"), id}})
                 .value(QLatin1String("ok")).toBool(),
             true);
    // A full send round trip without JS in the loop: the pending mid
    // resolves through the host's relay response.
    QSignalSpy eventSpy(bridge, &ExtensionPageBridge::extEvent);
    const QString mid = id + QStringLiteral("#direct#1");
    const QJsonObject send = bridge->invoke(
        QStringLiteral("send"),
        QJsonObject{{QStringLiteral("ext"), id},
                    {QStringLiteral("mid"), mid},
                    {QStringLiteral("msg"),
                     QJsonObject{{QStringLiteral("greet"),
                                  QStringLiteral("hi")}}},
                    {QStringLiteral("senderUrl"),
                     page->url().toString()}});
    QVERIFY2(send.value(QLatin1String("ok")).toBool(),
             qPrintable(send.value(QLatin1String("error"))
                            .toString()));
    QVERIFY(send.value(QLatin1String("result")).toObject()
                .value(QLatin1String("peers")).toInt() >= 1);
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            for (const QList<QVariant> &args : eventSpy) {
                if (args.value(0).toString()
                        != QLatin1String("__arktos.extResponse"))
                    continue;
                if (args.value(1).toJsonObject()
                        .value(QLatin1String("mid")).toString() == mid)
                    return true;
            }
            return false;
        }(), 20000);
    for (const QList<QVariant> &args : eventSpy) {
        if (args.value(1).toJsonObject()
                .value(QLatin1String("mid")).toString() != mid)
            continue;
        const QJsonObject response = args.value(1).toJsonObject()
            .value(QLatin1String("response")).toObject();
        QCOMPARE(response.value(QLatin1String("echo")).toObject()
                     .value(QLatin1String("greet")).toString(),
                 QStringLiteral("hi"));
    }

    // --- the gate: typed errors, never silence ----------------------
    // A foreign extension id (not hosted, no grants) gets the
    // absent-API reply even on a covered page.
    QCOMPARE(bridge->invoke(
                 QStringLiteral("hello"),
                 QJsonObject{{QStringLiteral("ext"),
                              QStringLiteral(
                                  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("no-host-access"));
    // A mid outside the extension's namespace never reaches routing.
    QCOMPARE(bridge->invoke(
                 QStringLiteral("send"),
                 QJsonObject{{QStringLiteral("ext"), id},
                             {QStringLiteral("mid"),
                              QStringLiteral("other#x#1")},
                             {QStringLiteral("msg"),
                              QJsonObject{}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-mid"));

    // --- an uncovered page: no chrome, no bridge answers -------------
    auto *view2 = new QWebEngineView;
    auto *page2 = new WebPage(
        BrowserApplication::instance()->webEngineProfile(), view2);
    view2->setPage(page2);
    QSignalSpy loadSpy2(page2, &QWebEnginePage::loadFinished);
    // qrc: is outside every match scheme — the agent registers but
    // stays inactive and the bridge refuses calls.
    page2->load(QUrl(QStringLiteral("qrc:/pip-player.html")));
    QTRY_VERIFY_WITH_TIMEOUT(loadSpy2.count() >= 1, 30000);
    ExtensionPageBridge *bridge2 =
        page2->findChild<ExtensionPageBridge *>();
    QVERIFY(bridge2);
    QCOMPARE(bridge2->invoke(
                 QStringLiteral("hello"),
                 QJsonObject{{QStringLiteral("ext"), id}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("no-host-access"));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(page2, QStringLiteral(
            "!!(window.__arktosExtAgents"
            "&&__arktosExtAgents['%1'])").arg(id),
            agentWorld).toBool(),
        15000);
    QCOMPARE(eval(page2, probe + QStringLiteral(".active"),
                  agentWorld).toBool(),
             false);

    view->deleteLater();
    view2->deleteLater();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    manager->setExtensionEnabled(id, false);
    QTRY_VERIFY_WITH_TIMEOUT(!runtime->host(id), 20000);
    manager->removeExtension(id);
    QDir(dir).removeRecursively();
}

// ER02 — chrome.tabs.sendMessage: the host->page direction.  A
// fixture extension's declared content script registers
// chrome.runtime.onMessage (except on a designated no-listener
// path); the hosted background calls chrome.tabs.sendMessage against
// a real browser tab.  The round trip must cross the C++ dispatch
// (tabId -> WebView -> ExtensionPageBridge -> '__arktos.deliver' ->
// agent 'respond' -> '__arktos.deliverResponse'), and every failure
// mode answers a typed error: wrong tabId, foreign mid, subframe
// addressing, and a page the extension's grants do not cover.
void tst_ExtensionManager::extensionTabMessaging()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");

    const QString dir = QDir::temp().filePath(
        QStringLiteral("tst-exttabmsg-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dir));
    {
        QFile f(dir + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-tabmsg\","
                "\"version\":\"1.0\",\"permissions\":[\"storage\","
                "\"tabs\"],\"host_permissions\":[\"<all_urls>\"],"
                "\"background\":{\"service_worker\":\"sw.js\"},"
                "\"content_scripts\":[{\"matches\":[\"<all_urls>\"],"
                "\"js\":[\"cs.js\"],\"run_at\":\"document_idle\"}]}");
        f.close();
        QFile w(dir + QLatin1String("/sw.js"));
        QVERIFY(w.open(QIODevice::WriteOnly));
        w.write(
            "chrome.runtime.onInstalled.addListener(function(){});\n");
        w.close();
        QFile c(dir + QLatin1String("/cs.js"));
        QVERIFY(c.open(QIODevice::WriteOnly));
        c.write(
            "window.__csSenders=[];\n"
            "if(location.pathname!=='/nolistener'){"
            "chrome.runtime.onMessage.addListener("
            "function(m,s,r){"
            " window.__csSenders.push("
            "  {id:(s&&s.id)||null,"
            "   url:(s&&s.url)||null,"
            "   origin:(s&&s.origin)||null,"
            "   hasTab:!!(s&&s.tab)});"
            " r({got:m}); return false; });}\n");
        c.close();
    }

    ExtensionManager *manager = ExtensionManager::instance();
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();
    auto loaded = std::make_shared<QString>();
    QObject::connect(manager, &ExtensionManager::extensionLoaded,
                     this, [loaded](
                         const ExtensionManager::ExtensionInfo &info) {
        if (info.name == QLatin1String("tst-tabmsg")
                && info.error.isEmpty())
            *loaded = info.id;
    });
    manager->grantConsent(dir);
    manager->loadExtension(dir);
    QTRY_VERIFY_WITH_TIMEOUT(!loaded->isEmpty(), 30000);
    const QString id = *loaded;
    manager->setExtensionEnabled(id, true);
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime->host(id)
            && runtime->host(id)->state()
                   == ExtensionBackgroundHost::State::Ready,
        45000);
    ExtensionBackgroundHost *host = runtime->host(id);
    QVERIFY(host && host->bridge());

    auto eval = [](QWebEnginePage *page, const QString &source,
                   quint32 worldId = 0) {
        auto result = std::make_shared<QVariant>();
        page->runJavaScript(
            source, worldId,
            [result](const QVariant &v) { *result = v; });
        QElapsedTimer t;
        t.start();
        while (!result->isValid() && t.elapsed() < 15000)
            QTest::qWait(50);
        return *result;
    };
    const quint32 agentWorld =
        ExtensionBackgroundRuntime::AgentWorldId;
    const QString probe = QStringLiteral(
        "((window.__arktosExtAgents&&__arktosExtAgents['%1'])"
        "||{active:false,bridgeUp:false})").arg(id);

    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    const QString base = QStringLiteral("http://127.0.0.1:%1")
        .arg(server.serverPort());
    QObject::connect(&server, &QTcpServer::newConnection,
                     this, [&]() {
        QTcpSocket *socket = server.nextPendingConnection();
        QObject::connect(socket, &QTcpSocket::readyRead,
                         socket, [socket]() {
            socket->readAll();
            const QByteArray body =
                "<html><body>tabmsg</body></html>";
            socket->write(
                "HTTP/1.1 200\r\nContent-Type: text/html\r\n"
                "Content-Length: "
                + QByteArray::number(body.size())
                + "\r\nConnection: close\r\n\r\n" + body);
            socket->disconnectFromHost();
        });
    });

    // A real tab — the tabId registry resolves only tabs a
    // BrowserMainWindow's TabWidget hosts.
    BrowserMainWindow *window = new BrowserMainWindow;
    TabWidget *tw = window->tabWidget();
    QVERIFY(tw && tw->count() >= 1);
    WebView *view = tw->webView(0);
    QVERIFY(view && view->page());
    QSignalSpy loadSpy(view->page(), &QWebEnginePage::loadFinished);
    view->load(QUrl(base + QStringLiteral("/agent")));
    QTRY_VERIFY_WITH_TIMEOUT(loadSpy.count() >= 1, 30000);
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(view->page(), probe + QStringLiteral(".bridgeUp"),
             agentWorld).toBool(),
        30000);
    const int tabId = ExtensionShimBackend::tabIdFor(view);
    QVERIFY(tabId > 0);

    // --- the host->page round trip ----------------------------------
    eval(host->page(), QStringLiteral(
        "self.__tsm=undefined;"
        "chrome.tabs.sendMessage(%1,{fill:'x',n:7},function(r){"
        "self.__tsm={r:(r===undefined)?null:r,"
        "err:(chrome.runtime.lastError"
        "   &&chrome.runtime.lastError.message)"
        "  ||((window.__arktosShim&&__arktosShim.lastError)"
        "     &&__arktosShim.lastError.message)||null};});0")
        .arg(tabId));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(host->page(), QStringLiteral("self.__tsm!==undefined"))
            .toBool(),
        20000);
    const QJsonObject tsm = QJsonDocument::fromJson(
        eval(host->page(),
             QStringLiteral("JSON.stringify(self.__tsm)"))
            .toString().toUtf8()).object();
    QVERIFY2(!tsm.value(QLatin1String("r")).isNull(),
             qPrintable(QStringLiteral("sendMessage failed: %1")
                 .arg(eval(host->page(), QStringLiteral(
                      "JSON.stringify(self.__tsm)")).toString())));
    QCOMPARE(tsm.value(QLatin1String("r")).toObject()
                 .value(QLatin1String("got")).toObject()
                 .value(QLatin1String("fill")).toString(),
             QStringLiteral("x"));
    QVERIFY(tsm.value(QLatin1String("err")).isNull());

    // The content script saw a Chrome-shaped extension-context
    // sender: its own extension id + extension origin, no .tab.
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(view->page(), QStringLiteral(
            "(window.__csSenders||[]).length>=1"),
            agentWorld).toBool(),
        10000);
    const QJsonObject sender = QJsonDocument::fromJson(
        eval(view->page(), QStringLiteral(
            "JSON.stringify(window.__csSenders[0])"),
            agentWorld).toString().toUtf8()).object();
    QCOMPARE(sender.value(QLatin1String("id")).toString(), id);
    QCOMPARE(sender.value(QLatin1String("origin")).toString(),
             QStringLiteral("chrome-extension://") + id);
    QCOMPARE(sender.value(QLatin1String("hasTab")).toBool(), false);

    // --- a covered page with no listener -> honest no-receiver -----
    WebView *view2 = tw->makeNewTab(false);
    QVERIFY(view2 && view2->page());
    QSignalSpy loadSpy2(view2->page(), &QWebEnginePage::loadFinished);
    view2->load(QUrl(base + QStringLiteral("/nolistener")));
    QTRY_VERIFY_WITH_TIMEOUT(loadSpy2.count() >= 1, 30000);
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(view2->page(), probe + QStringLiteral(".bridgeUp"),
             agentWorld).toBool(),
        30000);
    const int tabId2 = ExtensionShimBackend::tabIdFor(view2);
    eval(host->page(), QStringLiteral(
        "self.__tsm2=undefined;self.__tsm2diag=undefined;"
        "try{"
        "var __sm=chrome.tabs.sendMessage;"
        "self.__tsm2diag={fn:typeof __sm,"
        "src:String(__sm&&__sm.toString()).slice(0,120)};"
        "chrome.tabs.sendMessage(%1,{x:1},function(r){"
        "try{self.__tsm2={r:(r===undefined)?'undef':'defined',"
        "err:(chrome.runtime.lastError"
        "   &&chrome.runtime.lastError.message)"
        "  ||((window.__arktosShim&&__arktosShim.lastError)"
        "     &&__arktosShim.lastError.message)||null};"
        "}catch(e){self.__tsm2={r:'cbthrew',err:String(e)};}});"
        "self.__tsm2diag.called=true;"
        "}catch(e){self.__tsm2diag.threw=String(e&&e.stack||e);}")
        .arg(tabId2));
    {
        QElapsedTimer wait2;
        wait2.start();
        while (!eval(host->page(),
                     QStringLiteral("self.__tsm2!==undefined"))
                    .toBool()
               && wait2.elapsed() < 20000)
            QTest::qWait(50);
        if (!eval(host->page(),
                  QStringLiteral("self.__tsm2!==undefined"))
                 .toBool()) {
            qWarning() << "TMSG DIAG host:"
                       << eval(host->page(), QStringLiteral(
                              "JSON.stringify(self.__tsm2diag)"))
                              .toString();
            qWarning() << "TMSG DIAG shim:"
                       << eval(host->page(), QStringLiteral(
                              "JSON.stringify(__arktosShim&&"
                              "{notes:(__arktosShim.notes||[])"
                              " .slice(-5),"
                              "errors:(__arktosShim.errors||[])"
                              " .slice(-5),"
                              "lastErr:__arktosShim.lastError,"
                              "touched:__arktosShim.touched}"
                              "||null)")).toString();
            eval(host->page(), QStringLiteral(
                "setTimeout(function(){self.__tprobe=1;},200);0"));
            QTest::qWait(1200);
            qWarning() << "TMSG DIAG timer:"
                       << eval(host->page(),
                               QStringLiteral("self.__tprobe||0"))
                              .toInt();
            qWarning() << "TMSG DIAG agent:"
                       << eval(view2->page(), QStringLiteral(
                              "JSON.stringify((window.__arktosExtAgents"
                              "&&__arktosExtAgents['%1']"
                              "&&{active:__arktosExtAgents['%1'].active,"
                              "deliveries:__arktosExtAgents['%1']"
                              "  .deliveries,"
                              "errors:__arktosExtAgents['%1'].errors})"
                              "||null)").arg(id),
                              agentWorld).toString();
        }
    }
    QVERIFY(eval(host->page(), QStringLiteral("self.__tsm2!==undefined"))
                .toBool());
    const QJsonObject tsm2 = QJsonDocument::fromJson(
        eval(host->page(),
             QStringLiteral("JSON.stringify(self.__tsm2)"))
            .toString().toUtf8()).object();
    QCOMPARE(tsm2.value(QLatin1String("r")).toString(),
             QStringLiteral("undef"));
    QVERIFY(tsm2.value(QLatin1String("err")).toString().contains(
        QStringLiteral("Could not establish connection")
        .left(10)));

    // --- the C++ gates, direct-driven ------------------------------
    ChromeBridge *bridge = host->bridge();
    // Wrong tab id — the registry has no such view.
    QCOMPARE(bridge->invoke(
                 QStringLiteral("tabs"),
                 QStringLiteral("sendMessage"),
                 QJsonObject{{QStringLiteral("id"), 424242},
                             {QStringLiteral("mid"),
                              id + QStringLiteral("#t#1")},
                             {QStringLiteral("msg"),
                              QJsonObject{}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("not-found"));
    // A mid outside the extension's namespace never routes.
    QCOMPARE(bridge->invoke(
                 QStringLiteral("tabs"),
                 QStringLiteral("sendMessage"),
                 QJsonObject{{QStringLiteral("id"), tabId},
                             {QStringLiteral("mid"),
                              QStringLiteral("other#x#1")},
                             {QStringLiteral("msg"),
                              QJsonObject{}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    // Subframe addressing is a documented gap — typed, not silently
    // delivered to frame 0.
    QCOMPARE(bridge->invoke(
                 QStringLiteral("tabs"),
                 QStringLiteral("sendMessage"),
                 QJsonObject{{QStringLiteral("id"), tabId},
                             {QStringLiteral("mid"),
                              id + QStringLiteral("#t#2")},
                             {QStringLiteral("msg"), QJsonObject{}},
                             {QStringLiteral("opts"),
                              QJsonObject{{QStringLiteral("frameId"),
                                           1}}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("unsupported"));

    // --- an uncovered page: the push never happens ------------------
    // The agentCovers gate runs BEFORE extEvent is emitted — an
    // uncovered document must not even see the repost.  qrc: sits
    // outside '<all_urls>'.
    WebView *view3 = tw->makeNewTab(false);
    QVERIFY(view3 && view3->page());
    QSignalSpy loadSpy3(view3->page(), &QWebEnginePage::loadFinished);
    view3->load(QUrl(QStringLiteral("qrc:/pip-player.html")));
    QTRY_VERIFY_WITH_TIMEOUT(loadSpy3.count() >= 1, 30000);
    const int tabId3 = ExtensionShimBackend::tabIdFor(view3);
    QCOMPARE(bridge->invoke(
                 QStringLiteral("tabs"),
                 QStringLiteral("sendMessage"),
                 QJsonObject{{QStringLiteral("id"), tabId3},
                             {QStringLiteral("mid"),
                              id + QStringLiteral("#t#3")},
                             {QStringLiteral("msg"),
                              QJsonObject{}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("no-host-access"));

    // --- the C++ delivery seam itself -------------------------------
    // A mid minted host-side: the extEvent push lands on the page
    // bridge, the agent's respond resolves it back to THIS bridge as
    // '__arktos.deliverResponse'.
    ExtensionPageBridge *pageBridge =
        view->page()->findChild<ExtensionPageBridge *>();
    QVERIFY(pageBridge);
    QSignalSpy pageSpy(pageBridge, &ExtensionPageBridge::extEvent);
    QSignalSpy hostSpy(bridge, &ChromeBridge::hostEvent);
    const QString mid = id + QStringLiteral("#direct#1");
    const QJsonObject accepted = bridge->invoke(
        QStringLiteral("tabs"), QStringLiteral("sendMessage"),
        QJsonObject{{QStringLiteral("id"), tabId},
                    {QStringLiteral("mid"), mid},
                    {QStringLiteral("msg"),
                     QJsonObject{{QStringLiteral("seam"),
                                  QStringLiteral("1")}}}});
    QVERIFY(accepted.value(QLatin1String("ok")).toBool());
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            for (const QList<QVariant> &a : pageSpy) {
                if (a.value(0).toString()
                        == QLatin1String("__arktos.deliver")
                    && a.value(1).toJsonObject()
                           .value(QLatin1String("mid")).toString()
                           == mid)
                    return true;
            }
            return false;
        }(), 20000);
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            for (const QList<QVariant> &a : hostSpy) {
                if (a.value(0).toString()
                        == QLatin1String("__arktos.deliverResponse")
                    && a.value(1).toJsonObject()
                           .value(QLatin1String("mid")).toString()
                           == mid)
                    return true;
            }
            return false;
        }(), 20000);
    for (const QList<QVariant> &a : hostSpy) {
        if (a.value(0).toString()
                != QLatin1String("__arktos.deliverResponse"))
            continue;
        const QJsonObject p = a.value(1).toJsonObject();
        if (p.value(QLatin1String("mid")).toString() != mid)
            continue;
        QCOMPARE(p.value(QLatin1String("response")).toObject()
                     .value(QLatin1String("got")).toObject()
                     .value(QLatin1String("seam")).toString(),
                 QStringLiteral("1"));
    }

    // deleteLater, not close(): three open tabs trip the
    // confirmClosingMultipleTabs QMessageBox, which is modal and
    // unanswerable headless.
    window->deleteLater();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    manager->setExtensionEnabled(id, false);
    QTRY_VERIFY_WITH_TIMEOUT(!runtime->host(id), 20000);
    manager->removeExtension(id);
    QDir(dir).removeRecursively();
}

// ER07 — chrome.scripting.executeScript.  A fixture extension
// declaring 'scripting' + <all_urls> drives the real call from its
// hosted background: func/args/files forms land in the shared agent
// world of a real browser tab and marshal Chrome-shaped results back
// (thenables awaited through the parked-result poll); allFrames +
// frameIds address real subframes.  The security legs prove the
// gates run BEFORE any source reaches a page — an uncovered tab gets
// a typed 'no-host-access' and an extension lacking the grant gets
// 'no-permission' — never an injection.
void tst_ExtensionManager::extensionScripting()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");

    const QString dir = QDir::temp().filePath(
        QStringLiteral("tst-extscript-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dir));
    {
        QFile f(dir + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-script\","
                "\"version\":\"1.0\",\"permissions\":[\"storage\","
                "\"scripting\"],\"host_permissions\":[\"<all_urls>\"],"
                "\"background\":{\"service_worker\":\"sw.js\"},"
                "\"content_scripts\":[{\"matches\":[\"<all_urls>\"],"
                "\"js\":[\"cs.js\"],\"run_at\":\"document_idle\"}]}");
        f.close();
        QFile w(dir + QLatin1String("/sw.js"));
        QVERIFY(w.open(QIODevice::WriteOnly));
        w.write(
            "chrome.runtime.onInstalled.addListener(function(){});\n");
        w.close();
        QFile c(dir + QLatin1String("/cs.js"));
        QVERIFY(c.open(QIODevice::WriteOnly));
        c.write("window.__csRan=true;\n");
        c.close();
        // files[] resolves against the unpacked bundle.
        QFile j(dir + QLatin1String("/inject.js"));
        QVERIFY(j.open(QIODevice::WriteOnly));
        j.write("window.__injFileRan=(window.__injFileRan||0)+1;\n");
        j.close();
    }
    // The consent/denial fixture: identical shape minus 'scripting'.
    const QString dirB = QDir::temp().filePath(
        QStringLiteral("tst-extscript-b-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dirB));
    {
        QFile f(dirB + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-script-b\","
                "\"version\":\"1.0\",\"permissions\":[\"storage\"],"
                "\"host_permissions\":[\"<all_urls>\"],"
                "\"background\":{\"service_worker\":\"sw.js\"}}");
        f.close();
        QFile w(dirB + QLatin1String("/sw.js"));
        QVERIFY(w.open(QIODevice::WriteOnly));
        w.write(
            "chrome.runtime.onInstalled.addListener(function(){});\n");
        w.close();
    }

    ExtensionManager *manager = ExtensionManager::instance();
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();
    auto loaded = std::make_shared<QString>();
    QObject::connect(manager, &ExtensionManager::extensionLoaded,
                     this, [loaded](
                         const ExtensionManager::ExtensionInfo &info) {
        if (info.name == QLatin1String("tst-script")
                && info.error.isEmpty())
            *loaded = info.id;
    });
    manager->grantConsent(dir);
    manager->loadExtension(dir);
    QTRY_VERIFY_WITH_TIMEOUT(!loaded->isEmpty(), 30000);
    const QString id = *loaded;
    manager->setExtensionEnabled(id, true);
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime->host(id)
            && runtime->host(id)->state()
                   == ExtensionBackgroundHost::State::Ready,
        45000);
    ExtensionBackgroundHost *host = runtime->host(id);
    QVERIFY(host && host->bridge());
    ChromeBridge *bridge = host->bridge();

    auto loadedB = std::make_shared<QString>();
    QObject::connect(manager, &ExtensionManager::extensionLoaded,
                     this, [loadedB](
                         const ExtensionManager::ExtensionInfo &info) {
        if (info.name == QLatin1String("tst-script-b")
                && info.error.isEmpty())
            *loadedB = info.id;
    });
    manager->grantConsent(dirB);
    manager->loadExtension(dirB);
    QTRY_VERIFY_WITH_TIMEOUT(!loadedB->isEmpty(), 30000);
    const QString idB = *loadedB;
    manager->setExtensionEnabled(idB, true);
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime->host(idB)
            && runtime->host(idB)->state()
                   == ExtensionBackgroundHost::State::Ready,
        45000);
    ExtensionBackgroundHost *hostB = runtime->host(idB);
    QVERIFY(hostB && hostB->bridge());

    auto eval = [](QWebEnginePage *page, const QString &source,
                   quint32 worldId = 0) {
        auto result = std::make_shared<QVariant>();
        page->runJavaScript(
            source, worldId,
            [result](const QVariant &v) { *result = v; });
        QElapsedTimer t;
        t.start();
        while (!result->isValid() && t.elapsed() < 15000)
            QTest::qWait(50);
        return *result;
    };
    const quint32 agentWorld =
        ExtensionBackgroundRuntime::AgentWorldId;
    const QString probe = QStringLiteral(
        "((window.__arktosExtAgents&&__arktosExtAgents['%1'])"
        "||{active:false,bridgeUp:false})").arg(id);

    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    const QString base = QStringLiteral("http://127.0.0.1:%1")
        .arg(server.serverPort());
    QObject::connect(&server, &QTcpServer::newConnection,
                     this, [&]() {
        QTcpSocket *socket = server.nextPendingConnection();
        QObject::connect(socket, &QTcpSocket::readyRead,
                         socket, [socket]() {
            const QByteArray req = socket->readAll();
            const QByteArray body =
                req.contains("GET /frame")
                    ? QByteArray("<html><head><title>frame-doc"
                                 "</title></head><body>f</body>"
                                 "</html>")
                    : QByteArray("<html><head><title>exec-target"
                                 "</title></head><body>exec"
                                 "<iframe src=\"/frame\"></iframe>"
                                 "</body></html>");
            socket->write(
                "HTTP/1.1 200\r\nContent-Type: text/html\r\n"
                "Content-Length: "
                + QByteArray::number(body.size())
                + "\r\nConnection: close\r\n\r\n" + body);
            socket->disconnectFromHost();
        });
    });

    BrowserMainWindow *window = new BrowserMainWindow;
    TabWidget *tw = window->tabWidget();
    QVERIFY(tw && tw->count() >= 1);
    WebView *view = tw->webView(0);
    QVERIFY(view && view->page());
    QSignalSpy loadSpy(view->page(), &QWebEnginePage::loadFinished);
    view->load(QUrl(base + QStringLiteral("/agent")));
    QTRY_VERIFY_WITH_TIMEOUT(loadSpy.count() >= 1, 30000);
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(view->page(), probe + QStringLiteral(".bridgeUp"),
             agentWorld).toBool(),
        30000);
    // The subframe must be committed before allFrames/frameIds
    // address it.
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(view->page(), QStringLiteral("window.length")).toInt()
            >= 1,
        15000);
    const int tabId = ExtensionShimBackend::tabIdFor(view);
    QVERIFY(tabId > 0);

    // --- func form: result + the agent-world chrome binding --------
    // The injected func's chrome.runtime.id resolves through the
    // closure preamble to THIS extension's scoped agent object —
    // the same surface declared content scripts get.
    eval(host->page(), QStringLiteral(
        "self.__ex1=undefined;"
        "chrome.scripting.executeScript("
        "{target:{tabId:%1},"
        "func:function(){return {"
        " title:document.title,"
        " chromeId:(typeof chrome!=='undefined'"
        "   &&chrome.runtime)?chrome.runtime.id:null}}},"
        "function(r){self.__ex1={r:r||null,"
        "err:(chrome.runtime.lastError"
        "   &&chrome.runtime.lastError.message)"
        "  ||((window.__arktosShim&&__arktosShim.lastError)"
        "     &&__arktosShim.lastError.message)||null};});0")
        .arg(tabId));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(host->page(),
             QStringLiteral("self.__ex1!==undefined")).toBool(),
        20000);
    const QJsonObject ex1 = QJsonDocument::fromJson(
        eval(host->page(),
             QStringLiteral("JSON.stringify(self.__ex1)"))
            .toString().toUtf8()).object();
    QVERIFY2(!ex1.value(QLatin1String("r")).isNull()
                 && !ex1.value(QLatin1String("r")).toArray()
                        .isEmpty(),
             qPrintable(QStringLiteral("executeScript failed: %1")
                 .arg(eval(host->page(), QStringLiteral(
                      "JSON.stringify(self.__ex1)")).toString())));
    const QJsonObject ex1r0 =
        ex1.value(QLatin1String("r")).toArray().at(0).toObject();
    QCOMPARE(ex1r0.value(QLatin1String("frameId")).toInt(), 0);
    QCOMPARE(ex1r0.value(QLatin1String("result")).toObject()
                 .value(QLatin1String("title")).toString(),
             QStringLiteral("exec-target"));
    QCOMPARE(ex1r0.value(QLatin1String("result")).toObject()
                 .value(QLatin1String("chromeId")).toString(),
             id);
    QVERIFY(ex1.value(QLatin1String("err")).isNull());

    // --- args marshal into the func + ExecutionWorld enum ----------
    eval(host->page(), QStringLiteral(
        "self.__ex2=undefined;"
        "self.__ex2w=(chrome.scripting.ExecutionWorld"
        "  &&chrome.scripting.ExecutionWorld.ISOLATED)||null;"
        "chrome.scripting.executeScript("
        "{target:{tabId:%1},args:['alpha',3],"
        "func:function(a,b){return a+'/'+b;}},"
        "function(r){self.__ex2={r:r||null,"
        "err:(window.__arktosShim&&__arktosShim.lastError"
        "   &&__arktosShim.lastError.message)||null};});0")
        .arg(tabId));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(host->page(),
             QStringLiteral("self.__ex2!==undefined")).toBool(),
        20000);
    const QJsonObject ex2 = QJsonDocument::fromJson(
        eval(host->page(),
             QStringLiteral("JSON.stringify(self.__ex2)"))
            .toString().toUtf8()).object();
    QCOMPARE(ex2.value(QLatin1String("r")).toArray().at(0).toObject()
                 .value(QLatin1String("result")).toString(),
             QStringLiteral("alpha/3"));
    QCOMPARE(eval(host->page(), QStringLiteral("self.__ex2w"))
                 .toString(),
             QStringLiteral("ISOLATED"));

    // --- a returned Promise is awaited (parked + polled) ------------
    eval(host->page(), QStringLiteral(
        "self.__ex3=undefined;"
        "chrome.scripting.executeScript("
        "{target:{tabId:%1},"
        "func:function(){return new Promise(function(res){"
        " setTimeout(function(){res('async-ok');},50);});}},"
        "function(r){self.__ex3={r:r||null};});0")
        .arg(tabId));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(host->page(),
             QStringLiteral("self.__ex3!==undefined")).toBool(),
        20000);
    QCOMPARE(QJsonDocument::fromJson(
                 eval(host->page(),
                      QStringLiteral("JSON.stringify(self.__ex3)"))
                     .toString().toUtf8()).object()
                 .value(QLatin1String("r")).toArray().at(0).toObject()
                 .value(QLatin1String("result")).toString(),
             QStringLiteral("async-ok"));

    // --- files[] reads bundle content into the same world ----------
    eval(host->page(), QStringLiteral(
        "self.__ex4=undefined;"
        "chrome.scripting.executeScript("
        "{target:{tabId:%1},files:['inject.js']},"
        "function(r){self.__ex4={r:r||null};});0")
        .arg(tabId));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(host->page(),
             QStringLiteral("self.__ex4!==undefined")).toBool(),
        20000);
    // The file ran in the shared agent world — a main-world probe
    // must NOT see it.
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(view->page(),
             QStringLiteral("window.__injFileRan"),
             agentWorld).toInt() == 1,
        10000);
    QVERIFY(eval(view->page(),
                 QStringLiteral("window.__injFileRan")).toInt() == 0);

    // --- allFrames + frameIds address real subframes ----------------
    eval(host->page(), QStringLiteral(
        "self.__ex5=undefined;"
        "chrome.scripting.executeScript("
        "{target:{tabId:%1,allFrames:true},"
        "func:function(){return location.pathname;}},"
        "function(r){self.__ex5={r:r||null};});0")
        .arg(tabId));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(host->page(),
             QStringLiteral("self.__ex5!==undefined")).toBool(),
        20000);
    const QJsonArray ex5r = QJsonDocument::fromJson(
        eval(host->page(),
             QStringLiteral("JSON.stringify(self.__ex5)"))
            .toString().toUtf8()).object()
        .value(QLatin1String("r")).toArray();
    QVERIFY2(ex5r.size() >= 2,
             qPrintable(QStringLiteral(
                 "allFrames yielded %1 results")
                 .arg(ex5r.size())));
    QCOMPARE(ex5r.at(0).toObject()
                 .value(QLatin1String("frameId")).toInt(), 0);
    QCOMPARE(ex5r.at(0).toObject()
                 .value(QLatin1String("result")).toString(),
             QStringLiteral("/agent"));
    QCOMPARE(ex5r.at(1).toObject()
                 .value(QLatin1String("frameId")).toInt(), 1);
    QCOMPARE(ex5r.at(1).toObject()
                 .value(QLatin1String("result")).toString(),
             QStringLiteral("/frame"));
    eval(host->page(), QStringLiteral(
        "self.__ex6=undefined;"
        "chrome.scripting.executeScript("
        "{target:{tabId:%1,frameIds:[1]},"
        "func:function(){return location.pathname;}},"
        "function(r){self.__ex6={r:r||null};});0")
        .arg(tabId));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(host->page(),
             QStringLiteral("self.__ex6!==undefined")).toBool(),
        20000);
    const QJsonArray ex6r = QJsonDocument::fromJson(
        eval(host->page(),
             QStringLiteral("JSON.stringify(self.__ex6)"))
            .toString().toUtf8()).object()
        .value(QLatin1String("r")).toArray();
    QCOMPARE(ex6r.size(), 1);
    QCOMPARE(ex6r.at(0).toObject()
                 .value(QLatin1String("frameId")).toInt(), 1);
    QCOMPARE(ex6r.at(0).toObject()
                 .value(QLatin1String("result")).toString(),
             QStringLiteral("/frame"));

    // --- a throwing func resolves with a per-frame error ------------
    // Chrome resolves once injection started — the throw surfaces as
    // the entry's error field, not a call rejection.
    eval(host->page(), QStringLiteral(
        "self.__ex7=undefined;"
        "chrome.scripting.executeScript("
        "{target:{tabId:%1},"
        "func:function(){throw new Error('boom');}},"
        "function(r){self.__ex7={r:r||null};});0")
        .arg(tabId));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(host->page(),
             QStringLiteral("self.__ex7!==undefined")).toBool(),
        20000);
    const QJsonObject ex7r0 = QJsonDocument::fromJson(
        eval(host->page(),
             QStringLiteral("JSON.stringify(self.__ex7)"))
            .toString().toUtf8()).object()
        .value(QLatin1String("r")).toArray().at(0).toObject();
    QVERIFY(ex7r0.value(QLatin1String("error")).toString()
                .contains(QStringLiteral("boom")));

    // --- host-permission miss: typed error BEFORE the page ---------
    // qrc: sits outside '<all_urls>' — the agentCovers gate must
    // refuse before a single byte is generated into the document.
    WebView *view3 = tw->makeNewTab(false);
    QVERIFY(view3 && view3->page());
    QSignalSpy loadSpy3(view3->page(), &QWebEnginePage::loadFinished);
    view3->load(QUrl(QStringLiteral("qrc:/pip-player.html")));
    QTRY_VERIFY_WITH_TIMEOUT(loadSpy3.count() >= 1, 30000);
    const int tabId3 = ExtensionShimBackend::tabIdFor(view3);
    eval(host->page(), QStringLiteral(
        "self.__ex8=undefined;"
        "chrome.scripting.executeScript("
        "{target:{tabId:%1},func:function(){return 1;}},"
        "function(r){self.__ex8={r:(r===undefined)?'undef':'def',"
        "err:(window.__arktosShim&&__arktosShim.lastError"
        "   &&__arktosShim.lastError.message)||null};});0")
        .arg(tabId3));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(host->page(),
             QStringLiteral("self.__ex8!==undefined")).toBool(),
        20000);
    const QJsonObject ex8 = QJsonDocument::fromJson(
        eval(host->page(),
             QStringLiteral("JSON.stringify(self.__ex8)"))
            .toString().toUtf8()).object();
    QCOMPARE(ex8.value(QLatin1String("r")).toString(),
             QStringLiteral("undef"));
    QVERIFY(ex8.value(QLatin1String("err")).toString()
                .contains(QStringLiteral("no-host-access")));

    // --- the C++ gates, direct-driven -------------------------------
    // 'scripting' permission absent -> refused before anything else.
    QCOMPARE(hostB->bridge()->invoke(
                 QStringLiteral("scripting"),
                 QStringLiteral("executeScript"),
                 QJsonObject{{QStringLiteral("execId"),
                              idB + QStringLiteral("#execX")},
                             {QStringLiteral("target"),
                              QJsonObject{{QStringLiteral("tabId"),
                                           tabId}}},
                             {QStringLiteral("func"),
                              QStringLiteral("function(){}")}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("no-permission"));
    // The execId prefix is a capability — a key outside the
    // extension's own namespace never runs.
    QCOMPARE(bridge->invoke(
                 QStringLiteral("scripting"),
                 QStringLiteral("executeScript"),
                 QJsonObject{{QStringLiteral("execId"),
                              QStringLiteral("other#e1")},
                             {QStringLiteral("target"),
                              QJsonObject{{QStringLiteral("tabId"),
                                           tabId}}},
                             {QStringLiteral("func"),
                              QStringLiteral("function(){}")}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    // No such tab.
    QCOMPARE(bridge->invoke(
                 QStringLiteral("scripting"),
                 QStringLiteral("executeScript"),
                 QJsonObject{{QStringLiteral("execId"),
                              id + QStringLiteral("#e1")},
                             {QStringLiteral("target"),
                              QJsonObject{{QStringLiteral("tabId"),
                                           424242}}},
                             {QStringLiteral("func"),
                              QStringLiteral("function(){}")}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("not-found"));
    // documentIds — a handle we never mint — is honest unsupported,
    // not a silent frame-0 substitution.
    QCOMPARE(bridge->invoke(
                 QStringLiteral("scripting"),
                 QStringLiteral("executeScript"),
                 QJsonObject{{QStringLiteral("execId"),
                              id + QStringLiteral("#e2")},
                             {QStringLiteral("target"),
                              QJsonObject{{QStringLiteral("tabId"),
                                           tabId},
                                          {QStringLiteral("documentIds"),
                                           QJsonArray{
                                               QStringLiteral("d1")}}}},
                             {QStringLiteral("func"),
                              QStringLiteral("function(){}")}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("unsupported"));
    // A frame id beyond the current tree.
    QCOMPARE(bridge->invoke(
                 QStringLiteral("scripting"),
                 QStringLiteral("executeScript"),
                 QJsonObject{{QStringLiteral("execId"),
                              id + QStringLiteral("#e3")},
                             {QStringLiteral("target"),
                              QJsonObject{{QStringLiteral("tabId"),
                                           tabId},
                                          {QStringLiteral("frameIds"),
                                           QJsonArray{77}}}},
                             {QStringLiteral("func"),
                              QStringLiteral("function(){}")}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("not-found"));
    // func+files together — Chrome takes one or the other.
    QCOMPARE(bridge->invoke(
                 QStringLiteral("scripting"),
                 QStringLiteral("executeScript"),
                 QJsonObject{{QStringLiteral("execId"),
                              id + QStringLiteral("#e4")},
                             {QStringLiteral("target"),
                              QJsonObject{{QStringLiteral("tabId"),
                                           tabId}}},
                             {QStringLiteral("func"),
                              QStringLiteral("function(){}")},
                             {QStringLiteral("files"),
                              QJsonArray{QStringLiteral("inject.js")}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    // A file outside the bundle's own tree never reads.
    QCOMPARE(bridge->invoke(
                 QStringLiteral("scripting"),
                 QStringLiteral("executeScript"),
                 QJsonObject{{QStringLiteral("execId"),
                              id + QStringLiteral("#e5")},
                             {QStringLiteral("target"),
                              QJsonObject{{QStringLiteral("tabId"),
                                           tabId}}},
                             {QStringLiteral("files"),
                              QJsonArray{QStringLiteral(
                                  "../manifest.json")}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("bad-path"));
    QCOMPARE(bridge->invoke(
                 QStringLiteral("scripting"),
                 QStringLiteral("executeScript"),
                 QJsonObject{{QStringLiteral("execId"),
                              id + QStringLiteral("#e6")},
                             {QStringLiteral("target"),
                              QJsonObject{{QStringLiteral("tabId"),
                                           tabId}}},
                             {QStringLiteral("files"),
                              QJsonArray{QStringLiteral(
                                  "missing.js")}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("no-such-file"));

    // The denial legs surface through the shim-gap ledger too — a
    // typed failure is recorded, never silent.
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            for (const QString &g : host->shimGaps()) {
                if (g.contains(QStringLiteral("scripting"))
                        && g.contains(QStringLiteral(
                               "no-host-access")))
                    return true;
            }
            return false;
        }(), 15000);

    window->deleteLater();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    manager->setExtensionEnabled(id, false);
    manager->setExtensionEnabled(idB, false);
    QTRY_VERIFY_WITH_TIMEOUT(!runtime->host(id), 20000);
    QTRY_VERIFY_WITH_TIMEOUT(!runtime->host(idB), 20000);
    manager->removeExtension(id);
    manager->removeExtension(idB);
    QDir(dir).removeRecursively();
    QDir(dirB).removeRecursively();
}

// ER08 — chrome.scripting.insertCSS/removeCSS + the executeScript
// multi-file form.  A fixture extension declaring 'scripting' +
// <all_urls> drives the real calls from its hosted background: the
// css form must produce a measurable style change on the page and
// removeCSS must restore it; the files form reads bundle css; the
// origin argument scopes which insertion a removeCSS lifts; allFrames
// reaches the iframe document.  The security legs prove the same
// gates executeScript carries — 'scripting' permission and
// host-permission coverage checked BEFORE any css reaches a page.
void tst_ExtensionManager::extensionScriptCss()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");

    const QString dir = QDir::temp().filePath(
        QStringLiteral("tst-extcss-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dir));
    {
        QFile f(dir + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-css\","
                "\"version\":\"1.0\",\"permissions\":[\"storage\","
                "\"scripting\"],\"host_permissions\":[\"<all_urls>\"],"
                "\"background\":{\"service_worker\":\"sw.js\"},"
                "\"content_scripts\":[{\"matches\":[\"<all_urls>\"],"
                "\"js\":[\"cs.js\"],\"run_at\":\"document_idle\"}]}");
        f.close();
        QFile w(dir + QLatin1String("/sw.js"));
        QVERIFY(w.open(QIODevice::WriteOnly));
        w.write(
            "chrome.runtime.onInstalled.addListener(function(){});\n");
        w.close();
        QFile c(dir + QLatin1String("/cs.js"));
        QVERIFY(c.open(QIODevice::WriteOnly));
        c.write("window.__csRan=true;\n");
        c.close();
        // files[] css + multi-file executeScript resolve against the
        // unpacked bundle.
        QFile s(dir + QLatin1String("/overlay.css"));
        QVERIFY(s.open(QIODevice::WriteOnly));
        s.write("#box{visibility:hidden}\n");
        s.close();
        QFile a(dir + QLatin1String("/a.js"));
        QVERIFY(a.open(QIODevice::WriteOnly));
        a.write("window.__fileA=(window.__fileA||0)+1;\n");
        a.close();
        QFile b(dir + QLatin1String("/b.js"));
        QVERIFY(b.open(QIODevice::WriteOnly));
        b.write("window.__fileB=window.__fileA+1;\n");
        b.close();
    }
    const QString dirB = QDir::temp().filePath(
        QStringLiteral("tst-extcss-b-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dirB));
    {
        QFile f(dirB + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-css-b\","
                "\"version\":\"1.0\",\"permissions\":[\"storage\"],"
                "\"host_permissions\":[\"<all_urls>\"],"
                "\"background\":{\"service_worker\":\"sw.js\"}}");
        f.close();
        QFile w(dirB + QLatin1String("/sw.js"));
        QVERIFY(w.open(QIODevice::WriteOnly));
        w.write(
            "chrome.runtime.onInstalled.addListener(function(){});\n");
        w.close();
    }

    ExtensionManager *manager = ExtensionManager::instance();
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();
    auto loaded = std::make_shared<QString>();
    QObject::connect(manager, &ExtensionManager::extensionLoaded,
                     this, [loaded](
                         const ExtensionManager::ExtensionInfo &info) {
        if (info.name == QLatin1String("tst-css")
                && info.error.isEmpty())
            *loaded = info.id;
    });
    manager->grantConsent(dir);
    manager->loadExtension(dir);
    QTRY_VERIFY_WITH_TIMEOUT(!loaded->isEmpty(), 30000);
    const QString id = *loaded;
    manager->setExtensionEnabled(id, true);
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime->host(id)
            && runtime->host(id)->state()
                   == ExtensionBackgroundHost::State::Ready,
        45000);
    ExtensionBackgroundHost *host = runtime->host(id);
    QVERIFY(host && host->bridge());
    ChromeBridge *bridge = host->bridge();

    auto loadedB = std::make_shared<QString>();
    QObject::connect(manager, &ExtensionManager::extensionLoaded,
                     this, [loadedB](
                         const ExtensionManager::ExtensionInfo &info) {
        if (info.name == QLatin1String("tst-css-b")
                && info.error.isEmpty())
            *loadedB = info.id;
    });
    manager->grantConsent(dirB);
    manager->loadExtension(dirB);
    QTRY_VERIFY_WITH_TIMEOUT(!loadedB->isEmpty(), 30000);
    const QString idB = *loadedB;
    manager->setExtensionEnabled(idB, true);
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime->host(idB)
            && runtime->host(idB)->state()
                   == ExtensionBackgroundHost::State::Ready,
        45000);
    ExtensionBackgroundHost *hostB = runtime->host(idB);
    QVERIFY(hostB && hostB->bridge());

    auto eval = [](QWebEnginePage *page, const QString &source,
                   quint32 worldId = 0) {
        auto result = std::make_shared<QVariant>();
        page->runJavaScript(
            source, worldId,
            [result](const QVariant &v) { *result = v; });
        QElapsedTimer t;
        t.start();
        while (!result->isValid() && t.elapsed() < 15000)
            QTest::qWait(50);
        return *result;
    };
    const quint32 agentWorld =
        ExtensionBackgroundRuntime::AgentWorldId;
    const QString probe = QStringLiteral(
        "((window.__arktosExtAgents&&__arktosExtAgents['%1'])"
        "||{active:false,bridgeUp:false})").arg(id);
    const QString display = QStringLiteral(
        "getComputedStyle(document.getElementById('box')).display");
    const QString subDisplay = QStringLiteral(
        "getComputedStyle("
        "window.frames[0].document.getElementById('box')).display");

    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    const QString base = QStringLiteral("http://127.0.0.1:%1")
        .arg(server.serverPort());
    QObject::connect(&server, &QTcpServer::newConnection,
                     this, [&]() {
        QTcpSocket *socket = server.nextPendingConnection();
        QObject::connect(socket, &QTcpSocket::readyRead,
                         socket, [socket]() {
            const QByteArray req = socket->readAll();
            const QByteArray body =
                req.contains("GET /frame")
                    ? QByteArray("<html><head><title>frame-doc"
                                 "</title></head><body>"
                                 "<div id=\"box\">f</div>"
                                 "</body></html>")
                    : QByteArray("<html><head><title>css-target"
                                 "</title></head><body>"
                                 "<div id=\"box\">exec</div>"
                                 "<iframe src=\"/frame\"></iframe>"
                                 "</body></html>");
            socket->write(
                "HTTP/1.1 200\r\nContent-Type: text/html\r\n"
                "Content-Length: "
                + QByteArray::number(body.size())
                + "\r\nConnection: close\r\n\r\n" + body);
            socket->disconnectFromHost();
        });
    });

    BrowserMainWindow *window = new BrowserMainWindow;
    TabWidget *tw = window->tabWidget();
    QVERIFY(tw && tw->count() >= 1);
    WebView *view = tw->webView(0);
    QVERIFY(view && view->page());
    QSignalSpy loadSpy(view->page(), &QWebEnginePage::loadFinished);
    view->load(QUrl(base + QStringLiteral("/agent")));
    QTRY_VERIFY_WITH_TIMEOUT(loadSpy.count() >= 1, 30000);
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(view->page(), probe + QStringLiteral(".bridgeUp"),
             agentWorld).toBool(),
        30000);
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(view->page(), QStringLiteral("window.length")).toInt()
            >= 1,
        15000);
    const int tabId = ExtensionShimBackend::tabIdFor(view);
    QVERIFY(tabId > 0);
    QCOMPARE(eval(view->page(), display).toString(),
             QStringLiteral("block"));

    // --- insertCSS(css) changes a measurable style -----------------
    eval(host->page(), QStringLiteral(
        "self.__c1=undefined;"
        "chrome.scripting.insertCSS("
        "{target:{tabId:%1},css:'#box{display:none}'},"
        "function(){self.__c1={"
        "err:(chrome.runtime.lastError"
        "   &&chrome.runtime.lastError.message)"
        "  ||((window.__arktosShim&&__arktosShim.lastError)"
        "     &&__arktosShim.lastError.message)||null};});0")
        .arg(tabId));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(host->page(),
             QStringLiteral("self.__c1!==undefined")).toBool(),
        20000);
    QVERIFY(QJsonDocument::fromJson(
                eval(host->page(),
                     QStringLiteral("JSON.stringify(self.__c1)"))
                    .toString().toUtf8()).object()
                .value(QLatin1String("err")).isNull());
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(view->page(), display).toString()
            == QStringLiteral("none"),
        10000);

    // --- removeCSS(css) restores it --------------------------------
    eval(host->page(), QStringLiteral(
        "self.__c2=undefined;"
        "chrome.scripting.removeCSS("
        "{target:{tabId:%1},css:'#box{display:none}'},"
        "function(){self.__c2={"
        "err:(window.__arktosShim&&__arktosShim.lastError"
        "   &&__arktosShim.lastError.message)||null};});0")
        .arg(tabId));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(host->page(),
             QStringLiteral("self.__c2!==undefined")).toBool(),
        20000);
    QVERIFY(QJsonDocument::fromJson(
                eval(host->page(),
                     QStringLiteral("JSON.stringify(self.__c2)"))
                    .toString().toUtf8()).object()
                .value(QLatin1String("err")).isNull());
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(view->page(), display).toString()
            == QStringLiteral("block"),
        10000);

    // --- files[] form + the promise flavour ------------------------
    eval(host->page(), QStringLiteral(
        "self.__c3=undefined;"
        "chrome.scripting.insertCSS("
        "{target:{tabId:%1},files:['overlay.css']})"
        ".then(function(){self.__c3={ok:1};},"
        "function(e){self.__c3={err:String(e&&e.message||e)};});0")
        .arg(tabId));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(host->page(),
             QStringLiteral("self.__c3!==undefined")).toBool(),
        20000);
    QVERIFY(QJsonDocument::fromJson(
                eval(host->page(),
                     QStringLiteral("JSON.stringify(self.__c3)"))
                    .toString().toUtf8()).object()
                .contains(QLatin1String("ok")));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(view->page(), QStringLiteral(
            "getComputedStyle(document.getElementById('box'))"
            ".visibility")).toString() == QStringLiteral("hidden"),
        10000);
    eval(host->page(), QStringLiteral(
        "self.__c4=undefined;"
        "chrome.scripting.removeCSS("
        "{target:{tabId:%1},files:['overlay.css']})"
        ".then(function(){self.__c4={ok:1};},"
        "function(e){self.__c4={err:String(e&&e.message||e)};});0")
        .arg(tabId));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(host->page(),
             QStringLiteral("self.__c4!==undefined")).toBool(),
        20000);
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(view->page(), QStringLiteral(
            "getComputedStyle(document.getElementById('box'))"
            ".visibility")).toString() == QStringLiteral("visible"),
        10000);

    // --- origin scopes the removal key ------------------------------
    // A USER-origin insert is only lifted by a USER-origin remove —
    // Chrome keys injected sheets by origin.
    eval(host->page(), QStringLiteral(
        "self.__c5=undefined;"
        "chrome.scripting.insertCSS("
        "{target:{tabId:%1},css:'#box{display:none}',origin:'USER'})"
        ".then(function(){self.__c5={ok:1};},"
        "function(e){self.__c5={err:String(e&&e.message||e)};});0")
        .arg(tabId));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(host->page(),
             QStringLiteral("self.__c5!==undefined")).toBool(),
        20000);
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(view->page(), display).toString()
            == QStringLiteral("none"),
        10000);
    eval(host->page(), QStringLiteral(
        "self.__c6=undefined;"
        "chrome.scripting.removeCSS("
        "{target:{tabId:%1},css:'#box{display:none}',origin:'AUTHOR'})"
        ".then(function(){self.__c6={ok:1};},"
        "function(e){self.__c6={err:String(e&&e.message||e)};});0")
        .arg(tabId));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(host->page(),
             QStringLiteral("self.__c6!==undefined")).toBool(),
        20000);
    // AUTHOR remove is a no-op against the USER sheet.
    QTest::qWait(400);
    QCOMPARE(eval(view->page(), display).toString(),
             QStringLiteral("none"));
    eval(host->page(), QStringLiteral(
        "self.__c7=undefined;"
        "chrome.scripting.removeCSS("
        "{target:{tabId:%1},css:'#box{display:none}',origin:'USER'})"
        ".then(function(){self.__c7={ok:1};},"
        "function(e){self.__c7={err:String(e&&e.message||e)};});0")
        .arg(tabId));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(host->page(),
             QStringLiteral("self.__c7!==undefined")).toBool(),
        20000);
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(view->page(), display).toString()
            == QStringLiteral("block"),
        10000);

    // --- removeCSS on never-inserted css resolves (Chrome no-op) ---
    eval(host->page(), QStringLiteral(
        "self.__c8=undefined;"
        "chrome.scripting.removeCSS("
        "{target:{tabId:%1},css:'#never-was-there{color:red}'})"
        ".then(function(){self.__c8={ok:1};},"
        "function(e){self.__c8={err:String(e&&e.message||e)};});0")
        .arg(tabId));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(host->page(),
             QStringLiteral("self.__c8!==undefined")).toBool(),
        20000);
    QVERIFY(QJsonDocument::fromJson(
                eval(host->page(),
                     QStringLiteral("JSON.stringify(self.__c8)"))
                    .toString().toUtf8()).object()
                .contains(QLatin1String("ok")));

    // --- allFrames reaches the iframe document ----------------------
    eval(host->page(), QStringLiteral(
        "self.__c9=undefined;"
        "chrome.scripting.insertCSS("
        "{target:{tabId:%1,allFrames:true},css:'#box{display:none}'})"
        ".then(function(){self.__c9={ok:1};},"
        "function(e){self.__c9={err:String(e&&e.message||e)};});0")
        .arg(tabId));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(host->page(),
             QStringLiteral("self.__c9!==undefined")).toBool(),
        20000);
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(view->page(), display).toString()
            == QStringLiteral("none")
            && eval(view->page(), subDisplay).toString()
                   == QStringLiteral("none"),
        10000);
    // A frameIds-scoped remove lifts only that frame's sheet.
    eval(host->page(), QStringLiteral(
        "self.__c10=undefined;"
        "chrome.scripting.removeCSS("
        "{target:{tabId:%1,frameIds:[0]},css:'#box{display:none}'})"
        ".then(function(){self.__c10={ok:1};},"
        "function(e){self.__c10={err:String(e&&e.message||e)};});0")
        .arg(tabId));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(host->page(),
             QStringLiteral("self.__c10!==undefined")).toBool(),
        20000);
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(view->page(), display).toString()
            == QStringLiteral("block")
            && eval(view->page(), subDisplay).toString()
                   == QStringLiteral("none"),
        10000);

    // --- executeScript files[] runs the list in order ---------------
    eval(host->page(), QStringLiteral(
        "self.__c11=undefined;"
        "chrome.scripting.executeScript("
        "{target:{tabId:%1},files:['a.js','b.js']})"
        ".then(function(){self.__c11={ok:1};},"
        "function(e){self.__c11={err:String(e&&e.message||e)};});0")
        .arg(tabId));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(host->page(),
             QStringLiteral("self.__c11!==undefined")).toBool(),
        20000);
    // b.js read __fileA set by a.js — ordering inside the closure.
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(view->page(), QStringLiteral("window.__fileB"),
             agentWorld).toInt() == 2,
        10000);

    // --- the C++ gates, direct-driven -------------------------------
    const QJsonObject target{{QStringLiteral("tabId"), tabId}};
    // 'scripting' permission absent -> refused before anything else.
    QCOMPARE(hostB->bridge()->invoke(
                 QStringLiteral("scripting"),
                 QStringLiteral("insertCSS"),
                 QJsonObject{{QStringLiteral("execId"),
                              idB + QStringLiteral("#c1")},
                             {QStringLiteral("target"), target},
                             {QStringLiteral("css"),
                              QStringLiteral("#box{}")}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("no-permission"));
    QCOMPARE(hostB->bridge()->invoke(
                 QStringLiteral("scripting"),
                 QStringLiteral("removeCSS"),
                 QJsonObject{{QStringLiteral("execId"),
                              idB + QStringLiteral("#c2")},
                             {QStringLiteral("target"), target},
                             {QStringLiteral("css"),
                              QStringLiteral("#box{}")}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("no-permission"));
    // css+files together / neither — Chrome takes exactly one.
    QCOMPARE(bridge->invoke(
                 QStringLiteral("scripting"),
                 QStringLiteral("insertCSS"),
                 QJsonObject{{QStringLiteral("execId"),
                              id + QStringLiteral("#c3")},
                             {QStringLiteral("target"), target},
                             {QStringLiteral("css"),
                              QStringLiteral("#box{}")},
                             {QStringLiteral("files"),
                              QJsonArray{QStringLiteral(
                                  "overlay.css")}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    QCOMPARE(bridge->invoke(
                 QStringLiteral("scripting"),
                 QStringLiteral("removeCSS"),
                 QJsonObject{{QStringLiteral("execId"),
                              id + QStringLiteral("#c4")},
                             {QStringLiteral("target"), target}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    // An origin outside AUTHOR/USER.
    QCOMPARE(bridge->invoke(
                 QStringLiteral("scripting"),
                 QStringLiteral("insertCSS"),
                 QJsonObject{{QStringLiteral("execId"),
                              id + QStringLiteral("#c5")},
                             {QStringLiteral("target"), target},
                             {QStringLiteral("css"),
                              QStringLiteral("#box{}")},
                             {QStringLiteral("origin"),
                              QStringLiteral("WEIRD")}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    // Bundle-path hygiene — traversal and missing files stay typed.
    QCOMPARE(bridge->invoke(
                 QStringLiteral("scripting"),
                 QStringLiteral("insertCSS"),
                 QJsonObject{{QStringLiteral("execId"),
                              id + QStringLiteral("#c6")},
                             {QStringLiteral("target"), target},
                             {QStringLiteral("files"),
                              QJsonArray{QStringLiteral(
                                  "../manifest.json")}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("bad-path"));
    QCOMPARE(bridge->invoke(
                 QStringLiteral("scripting"),
                 QStringLiteral("insertCSS"),
                 QJsonObject{{QStringLiteral("execId"),
                              id + QStringLiteral("#c7")},
                             {QStringLiteral("target"), target},
                             {QStringLiteral("files"),
                              QJsonArray{QStringLiteral(
                                  "missing.css")}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("no-such-file"));
    // No such tab / a frame id beyond the tree / documentIds.
    QCOMPARE(bridge->invoke(
                 QStringLiteral("scripting"),
                 QStringLiteral("insertCSS"),
                 QJsonObject{{QStringLiteral("execId"),
                              id + QStringLiteral("#c8")},
                             {QStringLiteral("target"),
                              QJsonObject{{QStringLiteral("tabId"),
                                           424242}}},
                             {QStringLiteral("css"),
                              QStringLiteral("#box{}")}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("not-found"));
    QCOMPARE(bridge->invoke(
                 QStringLiteral("scripting"),
                 QStringLiteral("insertCSS"),
                 QJsonObject{{QStringLiteral("execId"),
                              id + QStringLiteral("#c9")},
                             {QStringLiteral("target"),
                              QJsonObject{{QStringLiteral("tabId"),
                                           tabId},
                                          {QStringLiteral("frameIds"),
                                           QJsonArray{77}}}},
                             {QStringLiteral("css"),
                              QStringLiteral("#box{}")}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("not-found"));
    QCOMPARE(bridge->invoke(
                 QStringLiteral("scripting"),
                 QStringLiteral("insertCSS"),
                 QJsonObject{{QStringLiteral("execId"),
                              id + QStringLiteral("#c10")},
                             {QStringLiteral("target"),
                              QJsonObject{{QStringLiteral("tabId"),
                                           tabId},
                                          {QStringLiteral("documentIds"),
                                           QJsonArray{
                                               QStringLiteral("d1")}}}},
                             {QStringLiteral("css"),
                              QStringLiteral("#box{}")}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("unsupported"));

    // --- an uncovered page: no css reaches it -----------------------
    // qrc: sits outside '<all_urls>' — the agentCovers gate must
    // refuse before a byte is generated into the document.
    WebView *view3 = tw->makeNewTab(false);
    QVERIFY(view3 && view3->page());
    QSignalSpy loadSpy3(view3->page(), &QWebEnginePage::loadFinished);
    view3->load(QUrl(QStringLiteral("qrc:/pip-player.html")));
    QTRY_VERIFY_WITH_TIMEOUT(loadSpy3.count() >= 1, 30000);
    const int tabId3 = ExtensionShimBackend::tabIdFor(view3);
    QCOMPARE(bridge->invoke(
                 QStringLiteral("scripting"),
                 QStringLiteral("insertCSS"),
                 QJsonObject{{QStringLiteral("execId"),
                              id + QStringLiteral("#c11")},
                             {QStringLiteral("target"),
                              QJsonObject{{QStringLiteral("tabId"),
                                           tabId3}}},
                             {QStringLiteral("css"),
                              QStringLiteral("#box{}")}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("no-host-access"));
    // Driven through the shim, the denial lands in the gap ledger.
    eval(host->page(), QStringLiteral(
        "self.__c12=undefined;"
        "chrome.scripting.insertCSS("
        "{target:{tabId:%1},css:'#box{display:none}'},"
        "function(){self.__c12={"
        "err:(window.__arktosShim&&__arktosShim.lastError"
        "   &&__arktosShim.lastError.message)||null};});0")
        .arg(tabId3));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(host->page(),
             QStringLiteral("self.__c12!==undefined")).toBool(),
        20000);
    QVERIFY(QJsonDocument::fromJson(
                eval(host->page(),
                     QStringLiteral("JSON.stringify(self.__c12)"))
                    .toString().toUtf8()).object()
                .value(QLatin1String("err")).toString()
                .contains(QStringLiteral("no-host-access")));
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            for (const QString &g : host->shimGaps()) {
                if (g.contains(QStringLiteral("insertCSS"))
                        && g.contains(QStringLiteral(
                               "no-host-access")))
                    return true;
            }
            return false;
        }(), 15000);

    window->deleteLater();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    manager->setExtensionEnabled(id, false);
    manager->setExtensionEnabled(idB, false);
    QTRY_VERIFY_WITH_TIMEOUT(!runtime->host(id), 20000);
    QTRY_VERIFY_WITH_TIMEOUT(!runtime->host(idB), 20000);
    manager->removeExtension(id);
    manager->removeExtension(idB);
    QDir(dir).removeRecursively();
    QDir(dirB).removeRecursively();
}

// SEC-EXT white-popup regression probe: replicate
// ExtensionActionBar::openPopup's EXACT construction — a view inside
// a popup frame, a plain page on the AMBIENT webEngineProfile() (not
// host->page()->profile()), view->load() with no explicit attach.
// The runtime's view watch must wire the bridge itself; a profile
// mismatch or a missed watch is the blank-popup failure mode.
void tst_ExtensionManager::actionPopupSelfWires()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");

    const QString dir = QDir::temp().filePath(
        QStringLiteral("tst-extselfwire-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dir));
    {
        QFile f(dir + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-selfwire\","
                "\"version\":\"1.0\",\"permissions\":[\"storage\"],"
                "\"action\":{\"default_popup\":\"popup.html\"},"
                "\"background\":{\"service_worker\":\"sw.js\"}}");
        f.close();
        QFile w(dir + QLatin1String("/sw.js"));
        QVERIFY(w.open(QIODevice::WriteOnly));
        w.write(
            "chrome.runtime.onInstalled.addListener(function(){});\n");
        w.close();
        QFile h(dir + QLatin1String("/popup.html"));
        QVERIFY(h.open(QIODevice::WriteOnly));
        h.write("<!doctype html><html><body><div id=\"r\">x</div>"
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
        if (info.name == QLatin1String("tst-selfwire")
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
    QVERIFY2(!id.isEmpty(), "fixture loaded");
    manager->setExtensionEnabled(id, true);
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime->host(id)
            && runtime->host(id)->state()
               == ExtensionBackgroundHost::State::Ready,
        45000);
    ExtensionBackgroundHost *host = runtime->host(id);
    QVERIFY(host && host->page());

    // The ambient profile openPopup uses MUST be the host's own —
    // otherwise the extension's documents don't even resolve there.
    QCOMPARE(BrowserApplication::instance()->webEngineProfile(),
             host->page()->profile());

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

    // openPopup verbatim: view inside a popup frame, plain page on
    // the ambient profile, load, no explicit attach.
    auto *frame = new QFrame(nullptr, Qt::Popup);
    auto *view = new QWebEngineView(frame);
    QWebEnginePage *page = new QWebEnginePage(
        BrowserApplication::instance()->webEngineProfile(), view);
    view->setPage(page);
    view->load(QUrl(QStringLiteral("chrome-extension://") + id
                    + QStringLiteral("/popup.html")));
    frame->show();

    // The watch attaches the page bridge once the document commits.
    QTRY_VERIFY_WITH_TIMEOUT(runtime->hasExtensionPage(page), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(page, QStringLiteral(
            "!!(window.__arktosHost&&__arktosHost.bridgeUp"
            "&&window.__arktosShim)")).toBool(),
        30000);
    QCOMPARE(eval(page, QStringLiteral(
                 "self.__popupBootRan===true")).toBool(),
             true);

    frame->close();
    view->deleteLater();
    frame->deleteLater();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    manager->setExtensionEnabled(id, false);
    QTRY_VERIFY_WITH_TIMEOUT(!runtime->host(id), 20000);
    manager->removeExtension(id);
    QDir(dir).removeRecursively();
}

// ER09 — dynamic content-script registration: chrome.scripting.
// registerContentScripts/unregister/get/update plus the Firefox-shape
// chrome.contentScripts.register/getScripts.  A fixture declaring
// 'scripting' + loopback host coverage drives the real calls from its
// hosted background: a registered spec must fire on the next matching
// navigation inside the agent world (never the page's own world),
// unregister must stop it, and a disable/enable cycle must re-arm the
// persisted set from ProfileSettings (session-only specs die).  The
// gate legs prove the same consent shape executeScript carries —
// 'scripting' permission for the MV3 surface, host-coverage
// implication for every registered match, and typed refusals
// (invalid-argument/bad-path/no-such-file/not-found/unsupported) all
// BEFORE a byte is armed.
void tst_ExtensionManager::extensionContentScriptRegistration()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");

    const QString dir = QDir::temp().filePath(
        QStringLiteral("tst-extdcs-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dir));
    {
        QFile f(dir + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-dcs\","
                "\"version\":\"1.0\",\"permissions\":[\"storage\","
                "\"scripting\"],\"host_permissions\":"
                "[\"http://127.0.0.1/*\"],\"background\":"
                "{\"service_worker\":\"sw.js\"},"
                "\"content_scripts\":[{\"matches\":"
                "[\"http://127.0.0.1/*\"],\"js\":[\"cs.js\"]}]}");
        f.close();
        QFile w(dir + QLatin1String("/sw.js"));
        QVERIFY(w.open(QIODevice::WriteOnly));
        w.write(
            "chrome.runtime.onInstalled.addListener(function(){});\n");
        w.close();
        QFile c(dir + QLatin1String("/cs.js"));
        QVERIFY(c.open(QIODevice::WriteOnly));
        c.write("window.__csRan=true;\n");
        c.close();
        // Registration candidates — counters so repeat navigations
        // measure whether a spec still fires.
        QFile a(dir + QLatin1String("/reg_a.js"));
        QVERIFY(a.open(QIODevice::WriteOnly));
        a.write("window.__regA=(window.__regA||0)+1;\n");
        a.close();
        QFile b(dir + QLatin1String("/reg_b.js"));
        QVERIFY(b.open(QIODevice::WriteOnly));
        b.write("window.__regB=(window.__regB||0)+1;\n");
        b.close();
        QFile c2(dir + QLatin1String("/reg_c.js"));
        QVERIFY(c2.open(QIODevice::WriteOnly));
        c2.write("window.__regC=(window.__regC||0)+1;\n");
        c2.close();
        QFile d(dir + QLatin1String("/reg_d.js"));
        QVERIFY(d.open(QIODevice::WriteOnly));
        d.write("window.__regD=(window.__regD||0)+1;\n");
        d.close();
    }
    // The consent fixture: identical coverage, no 'scripting'
    // permission — MV3 registration must refuse, the Firefox-shape
    // surface must still work (it has no API permission in Firefox
    // either).
    const QString dirB = QDir::temp().filePath(
        QStringLiteral("tst-extdcs-b-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dirB));
    {
        QFile f(dirB + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-dcs-b\","
                "\"version\":\"1.0\",\"permissions\":[\"storage\"],"
                "\"host_permissions\":[\"http://127.0.0.1/*\"],"
                "\"background\":{\"service_worker\":\"sw.js\"}}");
        f.close();
        QFile w(dirB + QLatin1String("/sw.js"));
        QVERIFY(w.open(QIODevice::WriteOnly));
        w.write(
            "chrome.runtime.onInstalled.addListener(function(){});\n");
        w.close();
        QFile a(dirB + QLatin1String("/reg_a.js"));
        QVERIFY(a.open(QIODevice::WriteOnly));
        a.write("window.__regA=(window.__regA||0)+1;\n");
        a.close();
    }

    ExtensionManager *manager = ExtensionManager::instance();
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();
    auto loaded = std::make_shared<QString>();
    QObject::connect(manager, &ExtensionManager::extensionLoaded,
                     this, [loaded](
                         const ExtensionManager::ExtensionInfo &info) {
        if (info.name == QLatin1String("tst-dcs")
                && info.error.isEmpty())
            *loaded = info.id;
    });
    manager->grantConsent(dir);
    manager->loadExtension(dir);
    QTRY_VERIFY_WITH_TIMEOUT(!loaded->isEmpty(), 30000);
    const QString id = *loaded;
    manager->setExtensionEnabled(id, true);
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime->host(id)
            && runtime->host(id)->state()
                   == ExtensionBackgroundHost::State::Ready,
        45000);
    ExtensionBackgroundHost *host = runtime->host(id);
    QVERIFY(host && host->bridge());
    ChromeBridge *bridge = host->bridge();

    auto loadedB = std::make_shared<QString>();
    QObject::connect(manager, &ExtensionManager::extensionLoaded,
                     this, [loadedB](
                         const ExtensionManager::ExtensionInfo &info) {
        if (info.name == QLatin1String("tst-dcs-b")
                && info.error.isEmpty())
            *loadedB = info.id;
    });
    manager->grantConsent(dirB);
    manager->loadExtension(dirB);
    QTRY_VERIFY_WITH_TIMEOUT(!loadedB->isEmpty(), 30000);
    const QString idB = *loadedB;
    manager->setExtensionEnabled(idB, true);
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime->host(idB)
            && runtime->host(idB)->state()
                   == ExtensionBackgroundHost::State::Ready,
        45000);
    ExtensionBackgroundHost *hostB = runtime->host(idB);
    QVERIFY(hostB && hostB->bridge());
    ChromeBridge *bridgeB = hostB->bridge();

    auto eval = [](QWebEnginePage *page, const QString &source,
                   quint32 worldId = 0) {
        auto result = std::make_shared<QVariant>();
        page->runJavaScript(
            source, worldId,
            [result](const QVariant &v) { *result = v; });
        QElapsedTimer t;
        t.start();
        while (!result->isValid() && t.elapsed() < 15000)
            QTest::qWait(50);
        return *result;
    };
    const quint32 agentWorld =
        ExtensionBackgroundRuntime::AgentWorldId;
    const QString probe = QStringLiteral(
        "((window.__arktosExtAgents&&__arktosExtAgents['%1'])"
        "||{active:false,bridgeUp:false})").arg(id);

    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    const QString base = QStringLiteral("http://127.0.0.1:%1")
        .arg(server.serverPort());
    QObject::connect(&server, &QTcpServer::newConnection,
                     this, [&]() {
        QTcpSocket *socket = server.nextPendingConnection();
        QObject::connect(socket, &QTcpSocket::readyRead,
                         socket, [socket]() {
            socket->readAll();
            const QByteArray body =
                "<html><head><title>reg-target</title></head>"
                "<body>reg</body></html>";
            socket->write(
                "HTTP/1.1 200\r\nContent-Type: text/html\r\n"
                "Content-Length: "
                + QByteArray::number(body.size())
                + "\r\nConnection: close\r\n\r\n" + body);
            socket->disconnectFromHost();
        });
    });

    BrowserMainWindow *window = new BrowserMainWindow;
    TabWidget *tw = window->tabWidget();
    QVERIFY(tw && tw->count() >= 1);
    WebView *view = tw->webView(0);
    QVERIFY(view && view->page());
    QSignalSpy loadSpy(view->page(), &QWebEnginePage::loadFinished);
    view->load(QUrl(base + QStringLiteral("/agent")));
    QTRY_VERIFY_WITH_TIMEOUT(loadSpy.count() >= 1, 30000);
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(view->page(), probe + QStringLiteral(".bridgeUp"),
             agentWorld).toBool(),
        30000);
    auto navigate = [&](const QString &path) {
        loadSpy.clear();
        view->load(QUrl(base + path));
        QTRY_VERIFY_WITH_TIMEOUT(loadSpy.count() >= 1, 30000);
        // The registered script's run wait caps at ~4s of agent
        // polling — the counter assertions below carry their own
        // QTRY windows.
    };

    // --- MV3 registerContentScripts → fires on next navigation -----
    eval(host->page(), QStringLiteral(
        "self.__r1=undefined;"
        "chrome.scripting.registerContentScripts([{id:'dyn1',"
        "matches:['http://127.0.0.1/*'],js:['reg_a.js'],"
        "runAt:'document_start'}])"
        ".then(function(){self.__r1='ok';},"
        "function(e){self.__r1='err:'+String(e&&e.message||e);});0"));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(host->page(), QStringLiteral("self.__r1")).toString()
            == QLatin1String("ok"),
        20000);
    // getRegisteredContentScripts echoes the armed spec.
    eval(host->page(), QStringLiteral(
        "self.__r2=undefined;"
        "chrome.scripting.getRegisteredContentScripts()"
        ".then(function(l){self.__r2=JSON.stringify(l);},"
        "function(e){self.__r2='err';});0"));
    QTRY_VERIFY_WITH_TIMEOUT(
        !eval(host->page(), QStringLiteral("self.__r2")).isNull(),
        20000);
    const QJsonArray listed = QJsonDocument::fromJson(
        eval(host->page(), QStringLiteral("self.__r2"))
            .toString().toUtf8()).array();
    QCOMPARE(listed.size(), 1);
    QCOMPARE(listed.at(0).toObject()
                 .value(QLatin1String("id")).toString(),
             QStringLiteral("dyn1"));
    QCOMPARE(listed.at(0).toObject()
                 .value(QLatin1String("js")).toArray().at(0)
                 .toString(),
             QStringLiteral("reg_a.js"));

    navigate(QStringLiteral("/agent?r1"));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(view->page(), QStringLiteral("window.__regA"),
             agentWorld).toInt() == 1,
        15000);
    // The registered script ran in the shared agent realm — the
    // page's own world must never see it.
    QCOMPARE(eval(view->page(), QStringLiteral("window.__regA"))
                 .toInt(), 0);

    // --- unregisterContentScripts stops the next navigation --------
    eval(host->page(), QStringLiteral(
        "self.__r3=undefined;"
        "chrome.scripting.unregisterContentScripts({ids:['dyn1']})"
        ".then(function(){self.__r3='ok';},"
        "function(e){self.__r3='err';});0"));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(host->page(), QStringLiteral("self.__r3")).toString()
            == QLatin1String("ok"),
        20000);
    // Each navigation is a fresh agent-world document — the counter
    // measures whether the spec fired on THIS navigation, so the
    // unregistered spec must leave it unset.
    navigate(QStringLiteral("/agent?r2"));
    QTest::qWait(1500);
    QCOMPARE(eval(view->page(), QStringLiteral("window.__regA"),
                  agentWorld).toInt(), 0);

    // --- the Firefox-shape surface ---------------------------------
    eval(host->page(), QStringLiteral(
        "self.__f1=undefined;"
        "chrome.contentScripts.register("
        "{matches:['http://127.0.0.1/*'],js:[{file:'reg_b.js'}]})"
        ".then(function(h){self.__f1=JSON.stringify("
        "{id:h.id,unreg:typeof h.unregister});},"
        "function(e){self.__f1='err:'+String(e&&e.message||e);});0"));
    QTRY_VERIFY_WITH_TIMEOUT(
        !eval(host->page(), QStringLiteral("self.__f1")).isNull(),
        20000);
    const QJsonObject handle = QJsonDocument::fromJson(
        eval(host->page(), QStringLiteral("self.__f1"))
            .toString().toUtf8()).object();
    QVERIFY2(handle.value(QLatin1String("id")).toString()
                 .startsWith(QLatin1String("mv2cs-")),
             qPrintable(eval(host->page(), QStringLiteral(
                             "self.__f1")).toString()));
    QCOMPARE(handle.value(QLatin1String("unreg")).toString(),
             QStringLiteral("function"));
    navigate(QStringLiteral("/agent?f1"));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(view->page(), QStringLiteral("window.__regB"),
             agentWorld).toInt() == 1,
        15000);
    // getScripts lists the MV2 handle; its own unregister() drops it.
    eval(host->page(), QStringLiteral(
        "self.__f2=undefined;"
        "chrome.contentScripts.getScripts()"
        ".then(function(l){self.__f2=JSON.stringify("
        "l.map(function(s){return s.id;}));},"
        "function(e){self.__f2='err';});0"));
    QTRY_VERIFY_WITH_TIMEOUT(
        !eval(host->page(), QStringLiteral("self.__f2")).isNull(),
        20000);
    QVERIFY(eval(host->page(), QStringLiteral("self.__f2"))
                .toString().contains(QStringLiteral("mv2cs-")));
    eval(host->page(), QStringLiteral(
        "self.__f3=undefined;"
        "chrome.contentScripts.getScripts()"
        ".then(function(l){return l[0].unregister();})"
        ".then(function(){self.__f3='ok';},"
        "function(e){self.__f3='err';});0"));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(host->page(), QStringLiteral("self.__f3")).toString()
            == QLatin1String("ok"),
        20000);
    navigate(QStringLiteral("/agent?f2"));
    QTest::qWait(1500);
    QCOMPARE(eval(view->page(), QStringLiteral("window.__regB"),
                  agentWorld).toInt(), 0);

    // --- persistence across a host respawn -------------------------
    // dyn3 persists (the Chrome default); dyn4 opts out and must die
    // with the extension's loaded lifetime.
    QCOMPARE(bridge->invoke(
                 QStringLiteral("scripting"),
                 QStringLiteral("registerContentScripts"),
                 QJsonObject{{QStringLiteral("scripts"), QJsonArray{
                     QJsonObject{{QStringLiteral("id"),
                                  QStringLiteral("dyn3")},
                                 {QStringLiteral("matches"),
                                  QJsonArray{QStringLiteral(
                                      "http://127.0.0.1/*")}},
                                 {QStringLiteral("js"),
                                  QJsonArray{QStringLiteral(
                                      "reg_c.js")}}},
                     QJsonObject{{QStringLiteral("id"),
                                  QStringLiteral("dyn4")},
                                 {QStringLiteral("matches"),
                                  QJsonArray{QStringLiteral(
                                      "http://127.0.0.1/*")}},
                                 {QStringLiteral("persistAcrossSessions"),
                                  false},
                                 {QStringLiteral("js"),
                                  QJsonArray{QStringLiteral(
                                      "reg_d.js")}}}}}})
                 .value(QLatin1String("ok")).toBool(),
             true);
    ProfileSettings regSettings(BrowserProfile::profileIdFor(
        host->page()->profile()));
    const QString regKey = QStringLiteral("extensions/regscripts/")
        + id;
    QVERIFY(regSettings.contains(regKey));
    const QString persisted = regSettings.value(regKey).toString();
    QVERIFY(persisted.contains(QStringLiteral("dyn3")));
    QVERIFY(!persisted.contains(QStringLiteral("dyn4")));

    manager->setExtensionEnabled(id, false);
    QTRY_VERIFY_WITH_TIMEOUT(!runtime->host(id), 20000);
    manager->setExtensionEnabled(id, true);
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime->host(id)
            && runtime->host(id)->state()
                   == ExtensionBackgroundHost::State::Ready,
        45000);
    host = runtime->host(id);
    bridge = host->bridge();
    // The persisted spec re-armed; the session one is gone.
    QVERIFY(runtime->registeredScriptSpecs(id)
                .contains(QStringLiteral("dyn3")));
    QVERIFY(!runtime->registeredScriptSpecs(id)
                 .contains(QStringLiteral("dyn4")));
    navigate(QStringLiteral("/agent?p1"));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(view->page(), QStringLiteral("window.__regC"),
             agentWorld).toInt() == 1,
        15000);
    QTest::qWait(1000);
    QCOMPARE(eval(view->page(), QStringLiteral("window.__regD"),
                  agentWorld).toInt(), 0);

    // --- updateContentScripts: the updated spec replaces the old ---
    QCOMPARE(bridge->invoke(
                 QStringLiteral("scripting"),
                 QStringLiteral("registerContentScripts"),
                 QJsonObject{{QStringLiteral("scripts"), QJsonArray{
                     QJsonObject{{QStringLiteral("id"),
                                  QStringLiteral("upd")},
                                 {QStringLiteral("matches"),
                                  QJsonArray{QStringLiteral(
                                      "http://127.0.0.1/*")}},
                                 {QStringLiteral("js"),
                                  QJsonArray{QStringLiteral(
                                      "reg_a.js")}}}}}})
                 .value(QLatin1String("ok")).toBool(),
             true);
    QCOMPARE(bridge->invoke(
                 QStringLiteral("scripting"),
                 QStringLiteral("updateContentScripts"),
                 QJsonObject{{QStringLiteral("scripts"), QJsonArray{
                     QJsonObject{{QStringLiteral("id"),
                                  QStringLiteral("upd")},
                                 {QStringLiteral("js"),
                                  QJsonArray{QStringLiteral(
                                      "reg_b.js")}}}}}})
                 .value(QLatin1String("ok")).toBool(),
             true);
    QCOMPARE(bridge->invoke(
                 QStringLiteral("scripting"),
                 QStringLiteral("getRegisteredContentScripts"),
                 QJsonObject{{QStringLiteral("filter"),
                              QJsonObject{{QStringLiteral("ids"),
                                           QJsonArray{QStringLiteral(
                                               "upd")}}}}})
                 .value(QLatin1String("result")).toArray()
                 .at(0).toObject()
                 .value(QLatin1String("js")).toArray().at(0)
                 .toString(),
             QStringLiteral("reg_b.js"));
    navigate(QStringLiteral("/agent?u1"));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(view->page(), QStringLiteral("window.__regB"),
             agentWorld).toInt() == 1,
        15000);

    // --- the typed gates, direct-driven ----------------------------
    // A match the extension never declared coverage for is refused
    // BEFORE anything is armed — the registered-pattern implication
    // check stands in for Chrome's "must have host permission".
    QCOMPARE(bridge->invoke(
                 QStringLiteral("scripting"),
                 QStringLiteral("registerContentScripts"),
                 QJsonObject{{QStringLiteral("scripts"), QJsonArray{
                     QJsonObject{{QStringLiteral("id"),
                                  QStringLiteral("bad1")},
                                 {QStringLiteral("matches"),
                                  QJsonArray{QStringLiteral(
                                      "https://elsewhere.example/*")}},
                                 {QStringLiteral("js"),
                                  QJsonArray{QStringLiteral(
                                      "reg_a.js")}}}}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("no-host-access"));
    // A batch that fails any entry registers nothing (Chrome's
    // atomic contract).
    QVERIFY(!runtime->registeredScriptSpecs(id)
                 .contains(QStringLiteral("bad1")));
    QCOMPARE(bridge->invoke(
                 QStringLiteral("scripting"),
                 QStringLiteral("registerContentScripts"),
                 QJsonObject{{QStringLiteral("scripts"), QJsonArray{
                     QJsonObject{{QStringLiteral("id"),
                                  QStringLiteral("bad2")},
                                 {QStringLiteral("matches"),
                                  QJsonArray{QStringLiteral(
                                      "http://127.0.0.1/*")}},
                                 {QStringLiteral("js"),
                                  QJsonArray{QStringLiteral(
                                      "../manifest.json")}}}}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("bad-path"));
    QCOMPARE(bridge->invoke(
                 QStringLiteral("scripting"),
                 QStringLiteral("registerContentScripts"),
                 QJsonObject{{QStringLiteral("scripts"), QJsonArray{
                     QJsonObject{{QStringLiteral("id"),
                                  QStringLiteral("bad3")},
                                 {QStringLiteral("matches"),
                                  QJsonArray{QStringLiteral(
                                      "http://127.0.0.1/*")}},
                                 {QStringLiteral("js"),
                                  QJsonArray{QStringLiteral(
                                      "missing.js")}}}}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("no-such-file"));
    // MV3 requires the caller id.
    QCOMPARE(bridge->invoke(
                 QStringLiteral("scripting"),
                 QStringLiteral("registerContentScripts"),
                 QJsonObject{{QStringLiteral("scripts"), QJsonArray{
                     QJsonObject{{QStringLiteral("matches"),
                                  QJsonArray{QStringLiteral(
                                      "http://127.0.0.1/*")}},
                                 {QStringLiteral("js"),
                                  QJsonArray{QStringLiteral(
                                      "reg_a.js")}}}}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    // A malformed match pattern.
    QCOMPARE(bridge->invoke(
                 QStringLiteral("scripting"),
                 QStringLiteral("registerContentScripts"),
                 QJsonObject{{QStringLiteral("scripts"), QJsonArray{
                     QJsonObject{{QStringLiteral("id"),
                                  QStringLiteral("bad4")},
                                 {QStringLiteral("matches"),
                                  QJsonArray{QStringLiteral(
                                      "not-a-pattern")}},
                                 {QStringLiteral("js"),
                                  QJsonArray{QStringLiteral(
                                      "reg_a.js")}}}}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    // Duplicate id — Chrome's nonexistent/duplicate contract.
    QCOMPARE(bridge->invoke(
                 QStringLiteral("scripting"),
                 QStringLiteral("registerContentScripts"),
                 QJsonObject{{QStringLiteral("scripts"), QJsonArray{
                     QJsonObject{{QStringLiteral("id"),
                                  QStringLiteral("upd")},
                                 {QStringLiteral("matches"),
                                  QJsonArray{QStringLiteral(
                                      "http://127.0.0.1/*")}},
                                 {QStringLiteral("js"),
                                  QJsonArray{QStringLiteral(
                                      "reg_a.js")}}}}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    QCOMPARE(bridge->invoke(
                 QStringLiteral("scripting"),
                 QStringLiteral("unregisterContentScripts"),
                 QJsonObject{{QStringLiteral("filter"),
                              QJsonObject{{QStringLiteral("ids"),
                                           QJsonArray{QStringLiteral(
                                               "nope")}}}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("not-found"));
    QCOMPARE(bridge->invoke(
                 QStringLiteral("scripting"),
                 QStringLiteral("updateContentScripts"),
                 QJsonObject{{QStringLiteral("scripts"), QJsonArray{
                     QJsonObject{{QStringLiteral("id"),
                                  QStringLiteral("nope")},
                                 {QStringLiteral("js"),
                                  QJsonArray{QStringLiteral(
                                      "reg_a.js")}}}}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("not-found"));
    // world:'MAIN' is accepted with the recorded approximation —
    // never silently re-scoped.
    const QJsonObject worldReg = bridge->invoke(
        QStringLiteral("scripting"),
        QStringLiteral("registerContentScripts"),
        QJsonObject{{QStringLiteral("scripts"), QJsonArray{
            QJsonObject{{QStringLiteral("id"),
                         QStringLiteral("m1")},
                        {QStringLiteral("matches"),
                         QJsonArray{QStringLiteral(
                             "http://127.0.0.1/*")}},
                        {QStringLiteral("world"),
                         QStringLiteral("MAIN")},
                        {QStringLiteral("js"),
                         QJsonArray{QStringLiteral(
                             "reg_a.js")}}}}}});
    QVERIFY(worldReg.value(QLatin1String("ok")).toBool());
    QVERIFY(worldReg.value(QLatin1String("result")).toObject()
                .value(QLatin1String("notes")).toArray()
                .size() >= 1);
    bridge->invoke(QStringLiteral("scripting"),
                   QStringLiteral("unregisterContentScripts"),
                   QJsonObject{{QStringLiteral("filter"),
                                QJsonObject{{QStringLiteral("ids"),
                                             QJsonArray{QStringLiteral(
                                                 "m1")}}}}});

    // Consent gates: the MV3 surface needs 'scripting'; the
    // Firefox-shape surface does not.
    QCOMPARE(bridgeB->invoke(
                 QStringLiteral("scripting"),
                 QStringLiteral("registerContentScripts"),
                 QJsonObject{{QStringLiteral("scripts"), QJsonArray{
                     QJsonObject{{QStringLiteral("id"),
                                  QStringLiteral("bx")},
                                 {QStringLiteral("matches"),
                                  QJsonArray{QStringLiteral(
                                      "http://127.0.0.1/*")}},
                                 {QStringLiteral("js"),
                                  QJsonArray{QStringLiteral(
                                      "reg_a.js")}}}}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("no-permission"));
    QVERIFY(bridgeB->invoke(
                QStringLiteral("contentScripts"),
                QStringLiteral("register"),
                QJsonObject{{QStringLiteral("options"),
                             QJsonObject{{QStringLiteral("matches"),
                                          QJsonArray{QStringLiteral(
                                              "http://127.0.0.1/*")}},
                                         {QStringLiteral("js"),
                                          QJsonArray{QJsonObject{
                                              {QStringLiteral("file"),
                                               QStringLiteral(
                                                   "reg_a.js")}}}}}}})
                .value(QLatin1String("ok")).toBool());
    QCOMPARE(bridgeB->invoke(
                 QStringLiteral("contentScripts"),
                 QStringLiteral("register"),
                 QJsonObject{{QStringLiteral("options"),
                              QJsonObject{{QStringLiteral("matches"),
                                           QJsonArray{QStringLiteral(
                                               "https://elsewhere.example/*")}},
                                          {QStringLiteral("js"),
                                           QJsonArray{QJsonObject{
                                               {QStringLiteral("file"),
                                                QStringLiteral(
                                                    "reg_a.js")}}}}}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("no-host-access"));
    QCOMPARE(bridgeB->invoke(
                 QStringLiteral("contentScripts"),
                 QStringLiteral("unregister"),
                 QJsonObject{{QStringLiteral("id"),
                              QStringLiteral("nope")}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("not-found"));
    // includeGlobs is a different matcher — typed unsupported, never
    // silently registered as-is.
    QCOMPARE(bridgeB->invoke(
                 QStringLiteral("contentScripts"),
                 QStringLiteral("register"),
                 QJsonObject{{QStringLiteral("options"),
                              QJsonObject{{QStringLiteral(
                                               "includeGlobs"),
                                           QJsonArray{QStringLiteral(
                                               "*x*")}}}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("unsupported"));

    window->deleteLater();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    manager->setExtensionEnabled(id, false);
    manager->setExtensionEnabled(idB, false);
    QTRY_VERIFY_WITH_TIMEOUT(!runtime->host(id), 20000);
    QTRY_VERIFY_WITH_TIMEOUT(!runtime->host(idB), 20000);
    manager->removeExtension(id);
    manager->removeExtension(idB);
    QDir(dir).removeRecursively();
    QDir(dirB).removeRecursively();
}

// Diagnostic leg for a REAL installed extension — pointed at an
// unpacked dir via ARKTOS_EXTTEST_DIR (e.g. the profile's Bitwarden).
// Loads it, waits for the hosted background, opens its action popup
// the way ExtensionActionBar does, and dumps the page's shim/bridge
// state + console-visible boot outcome.
void tst_ExtensionManager::realExtensionPopup()
{
    const QString dir =
        qEnvironmentVariable("ARKTOS_EXTTEST_DIR");
    if (dir.isEmpty())
        QSKIP("set ARKTOS_EXTTEST_DIR to an unpacked extension dir");
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");

    ExtensionManager *manager = ExtensionManager::instance();
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();
    auto loaded = std::make_shared<QString>();
    QObject::connect(manager, &ExtensionManager::extensionLoaded,
                     this, [loaded](
                         const ExtensionManager::ExtensionInfo &info) {
        if (info.error.isEmpty())
            *loaded = info.id;
    });
    manager->grantConsent(dir);
    manager->loadExtension(dir);
    QElapsedTimer timer;
    timer.start();
    while (loaded->isEmpty() && timer.elapsed() < 60000)
        QTest::qWait(100);
    const QString id = *loaded;
    QVERIFY2(!id.isEmpty(), "real extension loaded");

    manager->setExtensionEnabled(id, true);
    ExtensionBackgroundHost *host = nullptr;
    QElapsedTimer hostTimer;
    hostTimer.start();
    while (hostTimer.elapsed() < 60000) {
        host = runtime->host(id);
        if (host && host->state()
                == ExtensionBackgroundHost::State::Ready)
            break;
        QTest::qWait(200);
    }
    QVERIFY2(host, "hosted background exists");
    if (host->state() != ExtensionBackgroundHost::State::Ready)
        qWarning() << "HOST STATE:" << int(host->state())
                   << "errors:" << runtime->contextsJson(id);

    // openPopup verbatim — ambient profile, plain page, no attach.
    auto *frame = new QFrame(nullptr, Qt::Popup);
    auto *view = new QWebEngineView(frame);
    QWebEnginePage *page = new QWebEnginePage(
        BrowserApplication::instance()->webEngineProfile(), view);
    view->setPage(page);
    QSignalSpy loadSpy(page, &QWebEnginePage::loadFinished);
    // Resolve the action popup URL like the action bar does.
    QString popupPath = QStringLiteral("popup/index.html");
    for (const ExtensionManager::ExtensionInfo &info
         : manager->extensions()) {
        if (info.id == id && info.actionPopupUrl.isValid()) {
            popupPath = info.actionPopupUrl.path();
            break;
        }
    }
    view->load(QUrl(QStringLiteral("chrome-extension://") + id
                    + popupPath));
    qWarning() << "MARKER: load issued";
    frame->resize(360, 500);
    frame->show();
    qWarning() << "MARKER: frame shown, waiting for loadFinished";
    connect(page, &QWebEnginePage::renderProcessTerminated,
            this, [](QWebEnginePage::RenderProcessTerminationStatus s,
                     int code) {
                qWarning() << "MARKER: renderProcessTerminated"
                           << int(s) << code;
            });

    QTRY_VERIFY_WITH_TIMEOUT(loadSpy.count() >= 1, 30000);
    qWarning() << "MARKER: loadFinished ok=" << loadSpy.last().at(0);
    QTest::qWait(6000);   // shim poll + sweep interval

    auto eval = [](QWebEnginePage *p, const QString &source) {
        auto result = std::make_shared<QVariant>();
        p->runJavaScript(source,
                         [result](const QVariant &v) {
                             *result = v; });
        QElapsedTimer t;
        t.start();
        while (!result->isValid() && t.elapsed() < 15000)
            QTest::qWait(50);
        return *result;
    };
    qWarning() << "POPUP STATE:"
               << eval(page, QStringLiteral(
                      "JSON.stringify({url:location.href,"
                      "title:document.title,"
                      "ready:document.readyState,"
                      "bodyLen:document.body"
                      "?document.body.innerHTML.length:-1,"
                      "bodyTxt:document.body?document.body.innerText"
                      ".slice(0,120):'',"
                      "chrome:typeof chrome,"
                      "runtime:typeof chrome!=='undefined'"
                      "&&!!chrome.runtime,"
                      "hostCfg:!!window.__ARKTOS_HOST_CONFIG,"
                      "host:window.__arktosHost?{state:__arktosHost"
                      ".state,up:__arktosHost.bridgeUp,"
                      "err:__arktosHost.errors.slice(-4)}:null})"))
                      .toString();
    qWarning() << "PAGE PROFILE MATCH:"
               << (page->profile() == host->page()->profile());
    qWarning() << "BRIDGE ATTACHED:" << runtime->hasExtensionPage(page);
    qWarning() << "HOST STATE INT:" << int(host->state());

    frame->close();
    view->deleteLater();
    frame->deleteLater();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    manager->setExtensionEnabled(id, false);
    manager->removeExtension(id);
}

// ER00: the lane's work-order measurement.  Env-gated like
// realExtensionPopup (ARKTOS_EXTTEST_DIR = the unpacked Bitwarden
// bundle, treated READ-ONLY — the leg fixture-copies it to a temp
// dir first), then INSTALLS the copy onto the sandboxed test
// profile, enables it, waits out the hosted background boot, opens
// the action popup the way ExtensionActionBar does, and records
// every typed shim gap the bundle emits during boot: the backend's
// forwarded list plus each context's __arktosShim.probe() (gaps,
// notes, touched, audit, page errors).  The report prints to the
// test log and, when ARKTOS_EXTTEST_OUT names a file, is written
// there verbatim — that file is the committed baseline the ER lane
// works down.
void tst_ExtensionManager::bitwardenGapBaseline()
{
    const QString src = qEnvironmentVariable("ARKTOS_EXTTEST_DIR");
    if (src.isEmpty())
        QSKIP("set ARKTOS_EXTTEST_DIR to an unpacked extension dir");
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");
    QVERIFY(QFile::exists(src + QLatin1String("/manifest.json")));

    // Fixture copy — installing (and the hosted runtime's storage
    // writes) must never touch the live profile's bundle.
    const QString copyRoot = QDir::temp().filePath(
        QStringLiteral("tst-bwgap-%1")
            .arg(QCoreApplication::applicationPid()));
    const QString dir = copyRoot + QLatin1String("/bundle");
    QVERIFY2(copyTree(src, dir), "fixture copy of the bundle failed");
    QVERIFY(QFile::exists(dir + QLatin1String("/manifest.json")));

    const ExtensionManager::Manifest manifest =
        ExtensionManager::inspectManifest(dir);
    QVERIFY(manifest.valid);

    ExtensionManager *manager = ExtensionManager::instance();
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();

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
                     this, [installed, finished](const QString &m) {
        *installed = QStringLiteral("ERROR:") + m;
        *finished = true;
    });
    manager->grantConsent(dir);
    manager->installExtension(dir);
    QElapsedTimer installTimer;
    installTimer.start();
    while (!*finished && installTimer.elapsed() < 90000)
        QTest::qWait(100);
    const QString id = *installed;
    QVERIFY2(!id.isEmpty() && !id.startsWith(QLatin1String("ERROR:")),
             qPrintable(QStringLiteral(
                 "real bundle must install: %1").arg(id)));

    // Hosted runtime on — Ready or Failed are both baseline data;
    // only a never-spawned host is a leg failure.
    manager->setExtensionEnabled(id, true);
    ExtensionBackgroundHost *host = nullptr;
    QElapsedTimer hostTimer;
    hostTimer.start();
    while (hostTimer.elapsed() < 120000) {
        host = runtime->host(id);
        if (host && host->state()
                != ExtensionBackgroundHost::State::Starting)
            break;
        QTest::qWait(250);
    }
    QVERIFY2(host, "hosted background never spawned");
    const QString hostState =
        host->state() == ExtensionBackgroundHost::State::Ready
            ? QStringLiteral("ready")
            : QStringLiteral("failed:") + host->lastError();

    auto eval = [](QWebEnginePage *p, const QString &source) {
        auto result = std::make_shared<QVariant>();
        p->runJavaScript(source,
                         [result](const QVariant &v) { *result = v; });
        QElapsedTimer t;
        t.start();
        while (!result->isValid() && t.elapsed() < 15000)
            QTest::qWait(50);
        return *result;
    };

    // Boot window — Bitwarden's background keeps initializing after
    // Ready (storage reads, listener registrations, retries against
    // unsupported surfaces).  Sample the backend gap list until it
    // holds steady, capped so a chatty extension cannot stall the leg.
    auto settle = [&host](int stableNeed, qint64 capMs) {
        int lastCount = -1;
        int stable = 0;
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < capMs) {
            const int count = host ? host->shimGaps().size() : 0;
            if (count == lastCount) {
                if (++stable >= stableNeed)
                    break;
            } else {
                stable = 0;
                lastCount = count;
            }
            QTest::qWait(1000);
        }
    };
    settle(4, 30000);

    // --- open the action popup the way ExtensionActionBar does -----
    QString popupPath = QStringLiteral("/popup/index.html");
    for (const ExtensionManager::ExtensionInfo &info
         : manager->extensions()) {
        if (info.id == id && info.actionPopupUrl.isValid()) {
            popupPath = info.actionPopupUrl.path();
            break;
        }
    }
    auto *frame = new QFrame(nullptr, Qt::Popup);
    auto *view = new QWebEngineView(frame);
    QWebEnginePage *page = new QWebEnginePage(
        BrowserApplication::instance()->webEngineProfile(), view);
    view->setPage(page);
    auto popupCrashed = std::make_shared<bool>(false);
    connect(page, &QWebEnginePage::renderProcessTerminated,
            this, [popupCrashed](
                QWebEnginePage::RenderProcessTerminationStatus s,
                int code) {
        *popupCrashed = true;
        qWarning() << "BASELINE popup renderProcessTerminated"
                   << int(s) << code;
    });
    frame->resize(360, 500);
    view->load(QUrl(QStringLiteral("chrome-extension://") + id
                    + popupPath));
    frame->show();
    bool popupBridged = false;
    {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 45000) {
            if (runtime->hasExtensionPage(page)) {
                const QVariant up = eval(page, QStringLiteral(
                    "!!(window.__arktosHost&&__arktosHost.bridgeUp)"));
                if (up.toBool()) {
                    popupBridged = true;
                    break;
                }
            }
            QTest::qWait(250);
        }
    }
    settle(5, 60000);   // popup boot + retried calls settle down

    // --- collect ----------------------------------------------------
    const QString probeJs = QStringLiteral(
        "JSON.stringify(function(){"
        " var p=(window.__arktosShim&&__arktosShim.probe)"
        "   ?__arktosShim.probe():null;"
        " var h=window.__arktosHost||null;"
        " return {probe:p,"
        " host:h?{state:h.state,bridgeUp:!!h.bridgeUp,kind:h.kind,"
        "   errors:h.errors.slice(0,60)}:null,"
        " doc:{ready:document.readyState,title:document.title,"
        "   url:location.href}};}())");
    const QJsonObject hostProbe = QJsonDocument::fromJson(
        eval(host->page(), probeJs).toString().toUtf8()).object();
    const QJsonObject popupProbe = QJsonDocument::fromJson(
        eval(page, probeJs).toString().toUtf8()).object();

    QStringList gaps;
    const auto addGaps = [&gaps](const QJsonArray &list) {
        for (const QJsonValue &v : list) {
            const QString g = v.toString();
            if (!g.isEmpty() && !gaps.contains(g))
                gaps.append(g);
        }
    };
    for (const QString &g : host->shimGaps())
        if (!gaps.contains(g))
            gaps.append(g);
    addGaps(hostProbe.value(QLatin1String("probe")).toObject()
                .value(QLatin1String("gaps")).toArray());
    addGaps(popupProbe.value(QLatin1String("probe")).toObject()
                .value(QLatin1String("gaps")).toArray());
    gaps.sort();

    QStringList report;
    report << QStringLiteral(
        "# ER00 — hosted-runtime boot baseline");
    report << QStringLiteral("generated: %1").arg(
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    report << QStringLiteral("bundle: %1 name=\"%2\" version=%3")
        .arg(QFileInfo(src).fileName(), manifest.name,
             manifest.version);
    report << QStringLiteral("installed id: %1").arg(id);
    report << QStringLiteral("host state: %1").arg(hostState);
    report << QStringLiteral("popup bridge: %1%2")
        .arg(popupBridged ? QStringLiteral("up")
                          : QStringLiteral("down"))
        .arg(*popupCrashed
                 ? QStringLiteral(" (render process terminated)")
                 : QString());
    {
        QStringList kinds;
        for (const QJsonValue &c : runtime->contextsJson(id))
            kinds.append(c.toObject()
                .value(QLatin1String("contextType")).toString());
        report << QStringLiteral("contexts: %1")
            .arg(kinds.join(QLatin1String(", ")));
    }
    for (const ExtensionManager::ExtensionInfo &info
         : manager->extensions()) {
        if (info.id != id)
            continue;
        QString health = QStringLiteral("unknown");
        switch (info.health) {
        case ExtensionManager::ExtensionHealth::Healthy:
            health = QStringLiteral("healthy");
            break;
        case ExtensionManager::ExtensionHealth::Limited:
            health = QStringLiteral("limited");
            break;
        case ExtensionManager::ExtensionHealth::NotSupported:
            health = QStringLiteral("not-supported");
            break;
        default:
            break;
        }
        report << QStringLiteral(
            "manager row: health=%1 hostState=%2 hostError=\"%3\"")
            .arg(health, info.hostState, info.hostError);
        report << QStringLiteral("  healthReason: %1")
            .arg(info.healthReason);
        report << QStringLiteral("  missingApis: %1")
            .arg(info.missingApis.join(QLatin1String(", ")));
        break;
    }
    report << QString();
    report << QStringLiteral(
        "## Typed shim gaps — the lane work order (%1 unique)")
        .arg(gaps.size());
    for (const QString &g : gaps)
        report << QStringLiteral("- ") + g;
    report << QString();

    const auto probeSection =
        [&report](const QString &title, const QJsonObject &wrap) {
        const QJsonObject probe = wrap.value(QLatin1String("probe"))
            .toObject();
        const QJsonObject hostObj = wrap.value(QLatin1String("host"))
            .toObject();
        const QJsonObject doc = wrap.value(QLatin1String("doc"))
            .toObject();
        report << QStringLiteral("## %1").arg(title);
        if (probe.isEmpty()) {
            report << QStringLiteral("(no __arktosShim.probe — the "
                                     "shim never landed here)");
            report << QStringLiteral("doc: ready=%1 title=\"%2\"")
                .arg(doc.value(QLatin1String("ready")).toString(),
                     doc.value(QLatin1String("title")).toString());
            report << QString();
            return;
        }
        report << QStringLiteral("doc: ready=%1 title=\"%2\" url=%3")
            .arg(doc.value(QLatin1String("ready")).toString(),
                 doc.value(QLatin1String("title")).toString(),
                 doc.value(QLatin1String("url")).toString());
        report << QStringLiteral("host: state=%1 bridgeUp=%2 kind=%3")
            .arg(hostObj.value(QLatin1String("state")).toString())
            .arg(hostObj.value(QLatin1String("bridgeUp")).toBool())
            .arg(hostObj.value(QLatin1String("kind")).toString());
        const QJsonObject touched = probe
            .value(QLatin1String("touched")).toObject();
        QStringList touchList;
        for (const QString &k : touched.keys())
            touchList.append(QStringLiteral("%1=%2")
                .arg(k).arg(touched.value(k).toInt()));
        touchList.sort();
        report << QStringLiteral("touched (%1): %2")
            .arg(touchList.size())
            .arg(touchList.join(QLatin1String(", ")));
        report << QStringLiteral("notes (%1):")
            .arg(probe.value(QLatin1String("notes")).toArray().size());
        for (const QJsonValue &v
             : probe.value(QLatin1String("notes")).toArray())
            report << QStringLiteral("- ") + v.toString();
        const QJsonArray missing = probe.value(QLatin1String("audit"))
            .toObject().value(QLatin1String("missing")).toArray();
        QStringList missList;
        for (const QJsonValue &v : missing)
            missList.append(v.toString());
        report << QStringLiteral("audit.missing (%1): %2")
            .arg(missList.size())
            .arg(missList.join(QLatin1String(", ")));
        const QJsonArray errors = hostObj
            .value(QLatin1String("errors")).toArray();
        report << QStringLiteral("errors (%1):").arg(errors.size());
        for (const QJsonValue &v : errors)
            report << QStringLiteral("- ") + v.toString();
        report << QString();
    };
    probeSection(QStringLiteral("background context"), hostProbe);
    probeSection(QStringLiteral("popup context"), popupProbe);

    for (const QString &line : report)
        qWarning().noquote() << line;

    const QString out = qEnvironmentVariable("ARKTOS_EXTTEST_OUT");
    if (!out.isEmpty()) {
        QFile file(out);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(report.join(QLatin1Char('\n')).toUtf8());
            file.write("\n");
            file.close();
        } else {
            qWarning() << "BASELINE could not write" << out;
        }
    }
    QVERIFY2(!gaps.isEmpty() || popupBridged
                 || host->state() == ExtensionBackgroundHost::State::Ready,
             "baseline leg produced no data — nothing measured");

    // --- teardown ---------------------------------------------------
    frame->close();
    view->deleteLater();
    frame->deleteLater();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    manager->setExtensionEnabled(id, false);
    QTRY_VERIFY_WITH_TIMEOUT(!runtime->host(id), 20000);
    manager->removeExtension(id);
    QTest::qWait(500);
    QDir(copyRoot).removeRecursively();
}

// ER06 — the lane's DECISION GATE, made executable.  The study's E3
// checkpoint: drive the REAL Bitwarden bundle through the ER01/02
// bridge and measure whether the content-script handshake crosses
// the world-emulation boundary.  Four assertions, each through real
// Bitwarden code on both ends:
//
//   1. cs->bg sendMessage — the declared
//      content/trigger-autofill-script-injection.js (document_start)
//      posts {command:'triggerAutofillScriptInjection'}; a gate
//      listener inside the REAL hosted background must see it with
//      sender.tab.id === our tab (the autofillService early-returns
//      without it — the field is the handshake's load-bearing part).
//   2. cs->bg connect — content-message-handler.js opens
//      chrome.runtime.connect({name:'autofill-injected-script-port'})
//      purely to observe disconnects; the real background's
//      runtime.onConnect must fire with the same Chrome sender.
//   3. the background ACTS on the handshake — its message router
//      dispatches to autofillService.injectAutofillScripts, which
//      calls chrome.scripting.executeScript; this runtime answers
//      the typed 'unsupported' gap (ER07 territory).  The gap
//      appearing only after the page load is the proof of dispatch.
//   4. bg->cs deliver + real cs reply — Bitwarden's
//      content-message-handler forwards the bg->cs commands
//      'bgUnlockPopoutOpened'/'addedCipher' back through sendMessage.
//      Driving one via chrome.tabs.sendMessage from the real
//      background context must come back through the real cs onto
//      real bg onMessage listeners — the full loop.
//
// PASS = all four cross; BLOCK = a measured isolation finding, which
// belongs in .devin/EXT-RUNTIME-SPIKE.md with the exact failing
// primitive — never a papered-over verdict.  The report prints to
// the test log and, when ARKTOS_EXTTEST_OUT names a file, is written
// there verbatim.
void tst_ExtensionManager::bitwardenHandshakeGate()
{
    const QString src = qEnvironmentVariable("ARKTOS_EXTTEST_DIR");
    if (src.isEmpty())
        QSKIP("set ARKTOS_EXTTEST_DIR to an unpacked extension dir");
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");
    QVERIFY(QFile::exists(src + QLatin1String("/manifest.json")));

    // Fixture copy — install + runtime storage writes never touch
    // the live profile's bundle.
    const QString copyRoot = QDir::temp().filePath(
        QStringLiteral("tst-bwgate-%1")
            .arg(QCoreApplication::applicationPid()));
    const QString dir = copyRoot + QLatin1String("/bundle");
    QVERIFY2(copyTree(src, dir), "fixture copy of the bundle failed");
    QVERIFY(QFile::exists(dir + QLatin1String("/manifest.json")));

    const ExtensionManager::Manifest manifest =
        ExtensionManager::inspectManifest(dir);
    QVERIFY(manifest.valid);

    ExtensionManager *manager = ExtensionManager::instance();
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();

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
                     this, [installed, finished](const QString &m) {
        *installed = QStringLiteral("ERROR:") + m;
        *finished = true;
    });
    manager->grantConsent(dir);
    manager->installExtension(dir);
    QElapsedTimer installTimer;
    installTimer.start();
    while (!*finished && installTimer.elapsed() < 90000)
        QTest::qWait(100);
    const QString id = *installed;
    QVERIFY2(!id.isEmpty() && !id.startsWith(QLatin1String("ERROR:")),
             qPrintable(QStringLiteral(
                 "real bundle must install: %1").arg(id)));

    manager->setExtensionEnabled(id, true);
    ExtensionBackgroundHost *host = nullptr;
    QElapsedTimer hostTimer;
    hostTimer.start();
    while (hostTimer.elapsed() < 120000) {
        host = runtime->host(id);
        if (host && host->state()
                != ExtensionBackgroundHost::State::Starting)
            break;
        QTest::qWait(250);
    }
    QVERIFY2(host, "hosted background never spawned");
    QVERIFY2(host->state() == ExtensionBackgroundHost::State::Ready,
             qPrintable(QStringLiteral(
                 "background host not ready: %1").arg(host->lastError())));

    auto eval = [](QWebEnginePage *p, const QString &source,
                   quint32 worldId = 0) {
        auto result = std::make_shared<QVariant>();
        p->runJavaScript(source, worldId,
                         [result](const QVariant &v) { *result = v; });
        QElapsedTimer t;
        t.start();
        while (!result->isValid() && t.elapsed() < 15000)
            QTest::qWait(50);
        return *result;
    };
    const quint32 agentWorld =
        ExtensionBackgroundRuntime::AgentWorldId;
    const QString probe = QStringLiteral(
        "((window.__arktosExtAgents&&__arktosExtAgents['%1'])"
        "||{active:false,bridgeUp:false})").arg(id);
    auto gateJson = [&eval, &host]() {
        return QJsonDocument::fromJson(
            eval(host->page(), QStringLiteral(
                "JSON.stringify(self.__gate||{msgs:[],ports:[]})"))
                .toString().toUtf8()).object();
    };
    auto gateHasMsg = [&gateJson](const QString &command, int tabId) {
        const QJsonArray msgs = gateJson()
            .value(QLatin1String("msgs")).toArray();
        for (const QJsonValue &v : msgs) {
            const QJsonObject m = v.toObject();
            if (m.value(QLatin1String("command")).toString() == command
                    && m.value(QLatin1String("tab")).toInt() == tabId)
                return true;
        }
        return false;
    };

    // --- gate probes inside the REAL background context --------------
    // Installed before the tab loads so every cs->bg crossing is
    // captured verbatim: __gate.msgs records (command, sender.tab.id,
    // sender.url/origin); __gate.ports records onConnect port name +
    // sender.  Bitwarden's own listeners coexist — these only watch.
    QVERIFY(eval(host->page(), QStringLiteral(
        "self.__gate={msgs:[],ports:[]};"
        "chrome.runtime.onMessage.addListener(function(m,s){"
        "try{self.__gate.msgs.push({command:(m&&m.command)||null,"
        "tab:(s&&s.tab&&s.tab.id)||null,"
        "url:(s&&s.url)||null,"
        "origin:(s&&s.origin)||null});}catch(e){}"
        "return false;});"
        "chrome.runtime.onConnect.addListener(function(p){"
        "try{self.__gate.ports.push({name:(p&&p.name)||null,"
        "tab:(p&&p.sender&&p.sender.tab&&p.sender.tab.id)||null,"
        "frameId:(p&&p.sender&&p.sender.frameId)||null});}catch(e){}});"
        "self.__gate.armed=1;0"))
        .toInt() == 0);
    // Snapshot the shim's per-API access counts so leg 3 measures a
    // delta, not ambient traffic.
    const QJsonObject touchedBefore = QJsonDocument::fromJson(
        eval(host->page(), QStringLiteral(
            "JSON.stringify((window.__arktosShim&&__arktosShim.probe)"
            "?__arktosShim.probe().touched:{})"))
            .toString().toUtf8()).object();

    // --- a real tab on a covered page ---------------------------------
    // sender.tab only stamps when the page resolves to a TabWidget's
    // WebView — the bridge's honest identity, and the field
    // Bitwarden's handshake cannot proceed without.
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    const QString base = QStringLiteral("http://127.0.0.1:%1")
        .arg(server.serverPort());
    QObject::connect(&server, &QTcpServer::newConnection,
                     this, [&]() {
        QTcpSocket *socket = server.nextPendingConnection();
        QObject::connect(socket, &QTcpSocket::readyRead,
                         socket, [socket]() {
            socket->readAll();
            const QByteArray body =
                "<html><body><form>"
                "<input type=text name=username>"
                "<input type=password name=password>"
                "</form></body></html>";
            socket->write(
                "HTTP/1.1 200\r\nContent-Type: text/html\r\n"
                "Content-Length: "
                + QByteArray::number(body.size())
                + "\r\nConnection: close\r\n\r\n" + body);
            socket->disconnectFromHost();
        });
    });

    BrowserMainWindow *window = new BrowserMainWindow;
    TabWidget *tw = window->tabWidget();
    QVERIFY(tw && tw->count() >= 1);
    WebView *view = tw->webView(0);
    QVERIFY(view && view->page());
    QSignalSpy loadSpy(view->page(), &QWebEnginePage::loadFinished);
    view->load(QUrl(base + QStringLiteral("/login")));
    QTRY_VERIFY_WITH_TIMEOUT(loadSpy.count() >= 1, 30000);
    const int tabId = ExtensionShimBackend::tabIdFor(view);
    QVERIFY(tabId > 0);
    // Bitwarden's agent registers + handshakes; its declared content
    // scripts (document_start) run inside the shared agent realm.
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(view->page(), probe + QStringLiteral(".bridgeUp"),
             agentWorld).toBool(),
        30000);

    // --- leg 1: cs->bg sendMessage -------------------------------------
    QTRY_VERIFY_WITH_TIMEOUT(
        gateHasMsg(QStringLiteral("triggerAutofillScriptInjection"),
                   tabId),
        45000);
    QVERIFY2(gateHasMsg(QStringLiteral("triggerAutofillScriptInjection"),
                        tabId),
             qPrintable(QStringLiteral(
                 "cs->bg sendMessage never arrived with sender.tab: %1")
                 .arg(eval(host->page(), QStringLiteral(
                      "JSON.stringify(self.__gate)")).toString())));

    // --- leg 2: cs->bg connect -----------------------------------------
    auto gateHasPort = [&gateJson](const QString &name, int tabId) {
        const QJsonArray ports = gateJson()
            .value(QLatin1String("ports")).toArray();
        for (const QJsonValue &v : ports) {
            const QJsonObject p = v.toObject();
            if (p.value(QLatin1String("name")).toString() == name
                    && p.value(QLatin1String("tab")).toInt() == tabId)
                return true;
        }
        return false;
    };
    QTRY_VERIFY_WITH_TIMEOUT(
        gateHasPort(QStringLiteral("autofill-injected-script-port"),
                    tabId),
        15000);

    // --- leg 3: the real background ACTED on the handshake -------------
    // injectAutofillScripts(sender.tab, sender.frameId) ->
    // scriptInjectorService.inject -> chrome.scripting.executeScript
    // (or the MV2-fallback chrome.tabs.executeScript).  The dispatch
    // proof is the touched delta — post-ER07 the call EXECUTES, so no
    // gap records on success; a typed 'unsupported'/gated gap is the
    // honest fallback when the real call degrades.  The calls are
    // async inside the bundle (account state, RxJS), so poll.
    // Touched keys differ by serving stub: fillNs counts
    // 'scripting.executeScript', the unsupportedNs proxy counts
    // 'chrome.scripting.executeScript'; the MV2 fallback lands under
    // tabs.* the same way.  Sum every *executeScript key's delta.
    auto injectCalls = [&]() {
        const QJsonObject now = QJsonDocument::fromJson(
            eval(host->page(), QStringLiteral(
                "JSON.stringify((window.__arktosShim"
                "&&__arktosShim.probe)"
                "?__arktosShim.probe().touched:{})"))
                .toString().toUtf8()).object();
        int delta = 0;
        for (const QString &k : now.keys()) {
            if (!k.endsWith(QLatin1String("executeScript")))
                continue;
            delta += now.value(k).toInt()
                     - touchedBefore.value(k).toInt();
        }
        return delta;
    };
    auto gapHas = [&host](const QString &needle) {
        for (const QString &g : host->shimGaps())
            if (g.contains(needle))
                return true;
        return false;
    };
    // The touched counter increments only inside the invoked method —
    // its delta certifies the call crossed, not just the access.
    QTRY_VERIFY_WITH_TIMEOUT(injectCalls() > 0, 30000);
    const int scriptingCalls = injectCalls();
    QVERIFY2(scriptingCalls > 0,
             qPrintable(QStringLiteral(
                 "executeScript never invoked — gaps: %1")
                 .arg(host->shimGaps().join(QLatin1String(", ")))));
    const bool injectTypedGap =
        gapHas(QLatin1String("executeScript"));

    // --- leg 4: bg->cs deliver + the real cs replies -------------------
    eval(host->page(), QStringLiteral(
        "chrome.tabs.sendMessage(%1,"
        "{command:'bgUnlockPopoutOpened'},function(){});0")
        .arg(tabId));
    QTRY_VERIFY_WITH_TIMEOUT(
        gateHasMsg(QStringLiteral("bgUnlockPopoutOpened"), tabId),
        30000);
    QVERIFY2(gateHasMsg(QStringLiteral("bgUnlockPopoutOpened"), tabId),
             qPrintable(QStringLiteral(
                 "bg->cs deliver or the real cs's forward-back failed: "
                 "%1").arg(eval(host->page(), QStringLiteral(
                     "JSON.stringify(self.__gate)")).toString())));

    // --- collect -------------------------------------------------------
    QStringList report;
    report << QStringLiteral(
        "# ER06 — hosted-runtime decision gate (Bitwarden handshake)");
    report << QStringLiteral("generated: %1").arg(
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    report << QStringLiteral("bundle: %1 name=\"%2\" version=%3")
        .arg(QFileInfo(src).fileName(), manifest.name,
             manifest.version);
    report << QStringLiteral("installed id: %1").arg(id);
    report << QStringLiteral("host state: ready");
    report << QStringLiteral("tab: id=%1 url=%2")
        .arg(tabId).arg(view->page()->url().toString());
    report << QStringLiteral("VERDICT: PASS — the cs<->bg handshake "
                             "crossed the world-emulation boundary "
                             "through real Bitwarden code on both "
                             "ends; no world-identity requirement "
                             "surfaced.");
    report << QString();
    report << QStringLiteral("## handshake legs");
    report << QStringLiteral(
        "- cs->bg sendMessage: triggerAutofillScriptInjection "
        "delivered with sender.tab.id=%1 — PASS").arg(tabId);
    report << QStringLiteral(
        "- cs->bg connect: autofill-injected-script-port accepted by "
        "the real onConnect with sender.tab.id=%1 — PASS").arg(tabId);
    report << QStringLiteral(
        "- bg dispatch: %1 executeScript call(s) "
        "(scripting/tabs) after the handshake — %2, PASS")
        .arg(scriptingCalls)
        .arg(injectTypedGap
                 ? QStringLiteral("typed gap recorded")
                 : QStringLiteral("executed, no gap"));
    report << QStringLiteral(
        "- bg->cs deliver + real cs forward-back: "
        "bgUnlockPopoutOpened round-tripped — PASS");
    report << QString();
    const QJsonObject agentWrap = QJsonDocument::fromJson(
        eval(view->page(), QStringLiteral(
            "JSON.stringify(function(){"
            "var a=window.__arktosExtAgents"
            "&&__arktosExtAgents['%1'];"
            "return a?{deliveries:a.deliveries,"
            "ports:Object.keys(a.ports||{}).length,"
            "probe:a.probe()}:null;}())").arg(id),
            agentWorld).toString().toUtf8()).object();
    const QJsonObject agentProbe =
        agentWrap.value(QLatin1String("probe")).toObject();
    {
        const QJsonObject touchedMap = QJsonDocument::fromJson(
            eval(host->page(), QStringLiteral(
                "JSON.stringify((window.__arktosShim"
                "&&__arktosShim.probe)"
                "?__arktosShim.probe().touched:{})"))
                .toString().toUtf8()).object();
        QStringList touched;
        for (const QString &k : touchedMap.keys())
            touched.append(QStringLiteral("%1=%2")
                .arg(k).arg(touchedMap.value(k).toInt()));
        touched.sort();
        report << QStringLiteral("## background touched: %1")
            .arg(touched.join(QLatin1String(", ")));
        report << QStringLiteral("## agent probe: active=%1 "
                                     "bridgeUp=%2 deliveries=%3 "
                                     "ports=%4")
            .arg(agentProbe.value(QLatin1String("active")).toBool())
            .arg(agentProbe.value(QLatin1String("bridgeUp")).toBool())
            .arg(agentWrap.value(QLatin1String("deliveries")).toInt())
            .arg(agentWrap.value(QLatin1String("ports")).toInt());
        report << QStringLiteral("agent gaps:");
        for (const QJsonValue &v
             : agentProbe.value(QLatin1String("gaps")).toArray())
            report << QStringLiteral("- ") + v.toString();
        report << QStringLiteral("agent errors:");
        for (const QJsonValue &v
             : agentProbe.value(QLatin1String("errors")).toArray())
            report << QStringLiteral("- ") + v.toString();
        report << QStringLiteral("host shimGaps:");
        for (const QString &g : host->shimGaps())
            report << QStringLiteral("- ") + g;
    }
    report << QString();
    report << QStringLiteral(
        "## gate capture (every cs->bg crossing)");
    for (const QJsonValue &v : gateJson()
             .value(QLatin1String("msgs")).toArray())
        report << QStringLiteral("- msg ") + QString::fromUtf8(
            QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));
    for (const QJsonValue &v : gateJson()
             .value(QLatin1String("ports")).toArray())
        report << QStringLiteral("- port ") + QString::fromUtf8(
            QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));

    // The unlock-init path on a fresh install: Bitwarden boots into
    // the logged-out init (no enrolled account), so the crypto
    // offscreen document is never requested — record what the
    // dependency WOULD answer rather than faking a flow.
    const QVariant offscreenReply = eval(host->page(),
        QStringLiteral(
            "JSON.stringify(function(){var out={supported:"
            "typeof chrome.offscreen!=='undefined'};"
            "try{chrome.offscreen.hasDocument(function(r){"
            "self.__offs={r:r,err:(chrome.runtime.lastError"
            "&&chrome.runtime.lastError.message)||((__arktosShim"
            "&&__arktosShim.lastError)&&__arktosShim.lastError"
            ".message)||null};});}catch(e){out.threw=String(e);}"
            "return out;}())"));
    QTest::qWait(800);
    const QVariant offscreenResult = eval(host->page(),
        QStringLiteral("JSON.stringify(self.__offs||null)"));
    report << QString();
    report << QStringLiteral(
        "## unlock-init dependency: chrome.offscreen");
    report << QStringLiteral("- surface: %1")
        .arg(offscreenReply.toString());
    report << QStringLiteral("- hasDocument() answer: %1")
        .arg(offscreenResult.toString());
    report << QStringLiteral(
        "- note: a fresh install has no enrolled account, so the "
        "real unlock-init (Argon2 in the offscreen document) is not "
        "reachable from the harness.  chrome.offscreen is the ER10 "
        "shim binding — hasDocument answers through the bridge "
        "(false with no live document) and createDocument spawns a "
        "hidden bridge-backed page; the fixture coverage for that "
        "lifecycle lives in extensionOffscreen().");

    for (const QString &line : report)
        qWarning().noquote() << line;

    const QString out = qEnvironmentVariable("ARKTOS_EXTTEST_OUT");
    if (!out.isEmpty()) {
        QFile file(out);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(report.join(QLatin1Char('\n')).toUtf8());
            file.write("\n");
            file.close();
        } else {
            qWarning() << "GATE could not write" << out;
        }
    }

    // --- teardown -----------------------------------------------------
    window->close();
    window->deleteLater();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    manager->setExtensionEnabled(id, false);
    QTRY_VERIFY_WITH_TIMEOUT(!runtime->host(id), 20000);
    manager->removeExtension(id);
    QTest::qWait(500);
    QDir(copyRoot).removeRecursively();
}

// ER10: chrome.offscreen — one hidden second-class host page per
// extension.  A fixture extension declares the 'offscreen'
// permission and drives the real flow from its background context:
// hasDocument → false, typed denials (non-bundle URL, unknown
// reason, empty reasons), createDocument spawning the hidden page
// at the declared bundle-relative URL with the full shim + own
// bridge + relay context, hasDocument → true, a JS probe inside the
// document resolving through ITS bridge, getContexts reporting
// OFFSCREEN_DOCUMENT with the recorded reasons, a duplicate create
// failing with Chrome's verbatim single-document message, and
// closeDocument releasing the page.  A second fixture without the
// permission gets the typed 'no-permission' denial.
void tst_ExtensionManager::extensionOffscreen()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");

    const QString root = QDir::temp().filePath(
        QStringLiteral("tst-extoff-%1")
            .arg(QCoreApplication::applicationPid()));
    const QString dir = writeExtension(
        root, QStringLiteral("tst-off"), QStringLiteral("1.0.0"),
        QString(), {QStringLiteral("offscreen")});
    QVERIFY(!dir.isEmpty());
    {
        QFile html(dir + QLatin1String("/off.html"));
        QVERIFY(html.open(QIODevice::WriteOnly));
        html.write("<!doctype html><html><head>"
                   "<script src=\"off.js\"></script>"
                   "</head><body>off</body></html>\n");
        html.close();
        QFile js(dir + QLatin1String("/off.js"));
        QVERIFY(js.open(QIODevice::WriteOnly));
        js.write("self.__offBoot=!!(chrome&&chrome.runtime"
                 "&&chrome.storage&&chrome.storage.local);\n");
        js.close();
    }
    const QString noPermDir = writeExtension(
        root, QStringLiteral("tst-offnp"), QStringLiteral("1.0.0"));
    QVERIFY(!noPermDir.isEmpty());

    ExtensionManager *manager = ExtensionManager::instance();
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();
    auto loaded = std::make_shared<QString>();
    auto loadedNp = std::make_shared<QString>();
    QObject::connect(manager, &ExtensionManager::extensionLoaded,
                     this, [loaded, loadedNp](
                         const ExtensionManager::ExtensionInfo &info) {
        if (info.name == QLatin1String("tst-off")
                && info.error.isEmpty())
            *loaded = info.id;
        if (info.name == QLatin1String("tst-offnp")
                && info.error.isEmpty())
            *loadedNp = info.id;
    });
    manager->grantConsent(dir);
    manager->loadExtension(dir);
    QElapsedTimer loadTimer;
    loadTimer.start();
    while (loaded->isEmpty() && loadTimer.elapsed() < 30000)
        QTest::qWait(50);
    const QString id = *loaded;
    QVERIFY2(!id.isEmpty(), "fixture extension must install");

    manager->setExtensionEnabled(id, true);
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime->host(id)
            && runtime->host(id)->state()
                   == ExtensionBackgroundHost::State::Ready,
        45000);
    ExtensionBackgroundHost *host = runtime->host(id);
    QVERIFY(host && host->page());

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
    auto evalJson = [&eval](QWebEnginePage *page,
                            const QString &source) {
        return QJsonDocument::fromJson(
            eval(page, source).toString().toUtf8()).object();
    };

    // Drive the whole create/deny/dup cycle from the background
    // context — the async probe lands its findings on self.__off.
    host->page()->runJavaScript(QStringLiteral(
        "self.__off={done:false};"
        "(async function(){"
        " var out={};"
        " out.hasBefore=await chrome.offscreen.hasDocument();"
        " try{"
        "  await chrome.offscreen.createDocument({url:"
        "   'https://evil.example/x.html',reasons:['DOM_SCRAPING'],"
        "   justification:'x'});"
        "  out.badUrl='resolved';"
        " }catch(e){out.badUrl=e.code||e.message;}"
        " try{"
        "  await chrome.offscreen.createDocument({url:'off.html',"
        "   reasons:['NO_SUCH_REASON'],justification:'x'});"
        "  out.badReason='resolved';"
        " }catch(e){out.badReason=e.code||e.message;}"
        " try{"
        "  await chrome.offscreen.createDocument({url:'off.html',"
        "   reasons:[],justification:'x'});"
        "  out.emptyReason='resolved';"
        " }catch(e){out.emptyReason=e.code||e.message;}"
        " try{"
        "  await chrome.offscreen.createDocument({url:'off.html',"
        "   reasons:[chrome.offscreen.Reason.TESTING,"
        "            chrome.offscreen.Reason.LOCAL_STORAGE],"
        "   justification:'er10 test'});"
        "  out.created=true;"
        " }catch(e){out.createErr=(e.code||'')+'|'+e.message;}"
        " out.hasAfter=await chrome.offscreen.hasDocument();"
        " try{"
        "  await chrome.offscreen.createDocument({url:'off.html',"
        "   reasons:['TESTING'],justification:'x'});"
        "  out.dup='resolved';"
        " }catch(e){out.dup=e.message;out.dupCode=e.code;}"
        " var ctx=await chrome.runtime.getContexts({});"
        " out.contexts=(ctx||[]).map(function(c){"
        "   return c.contextType;});"
        " self.__off={done:true,out:out};"
        "})().catch(function(e){"
        " self.__off={done:true,fatal:String(e&&e.message||e)};});"));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(host->page(), QStringLiteral(
            "!!(self.__off&&self.__off.done)")).toBool(),
        30000);
    const QJsonObject probe = evalJson(
        host->page(), QStringLiteral("JSON.stringify(self.__off)"));
    QVERIFY2(!probe.contains(QLatin1String("fatal")),
             qPrintable(QString::fromUtf8(
                 QJsonDocument(probe).toJson(
                     QJsonDocument::Compact))));
    const QJsonObject out =
        probe.value(QLatin1String("out")).toObject();

    QCOMPARE(out.value(QLatin1String("hasBefore")).toBool(), false);
    QCOMPARE(out.value(QLatin1String("badUrl")).toString(),
             QStringLiteral("bad-path"));
    QCOMPARE(out.value(QLatin1String("badReason")).toString(),
             QStringLiteral("invalid-argument"));
    QCOMPARE(out.value(QLatin1String("emptyReason")).toString(),
             QStringLiteral("invalid-argument"));
    QVERIFY2(out.value(QLatin1String("created")).toBool(),
             qPrintable(out.value(QLatin1String("createErr"))
                            .toString()));
    QCOMPARE(out.value(QLatin1String("hasAfter")).toBool(), true);
    // Chrome's verbatim single-document message — Bitwarden's
    // withDocument compares e.message against this string.
    QCOMPARE(out.value(QLatin1String("dupCode")).toString(),
             QStringLiteral("single-document"));
    QCOMPARE(out.value(QLatin1String("dup")).toString(),
             QStringLiteral(
                 "Only a single offscreen document may be created."));
    QVERIFY(out.value(QLatin1String("contexts")).toArray().contains(
        QJsonValue(QStringLiteral("OFFSCREEN_DOCUMENT"))));

    // The runtime reports the same truth host-side.
    QVERIFY(runtime->offscreenHasDocument(id));
    const QJsonObject info = runtime->offscreenInfo(id);
    QCOMPARE(info.value(QLatin1String("url")).toString(),
             QStringLiteral("chrome-extension://") + id
                 + QStringLiteral("/off.html"));
    QCOMPARE(info.value(QLatin1String("justification")).toString(),
             QStringLiteral("er10 test"));
    const QJsonArray reasons =
        info.value(QLatin1String("reasons")).toArray();
    QVERIFY(reasons.contains(
        QJsonValue(QStringLiteral("TESTING"))));
    QVERIFY(reasons.contains(
        QJsonValue(QStringLiteral("LOCAL_STORAGE"))));

    // The spawned page: own bridge + shim live, its own script ran
    // (not the background worker's), and a chrome.storage call from
    // inside it round-trips through ITS bridge — proof the hidden
    // page is a real extension context, not a bare webview.
    QWebEnginePage *offPage = runtime->offscreenPage(id);
    QVERIFY(offPage);
    QCOMPARE(offPage->url().toString(),
             QStringLiteral("chrome-extension://") + id
                 + QStringLiteral("/off.html"));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(offPage, QStringLiteral(
            "!!(window.__arktosHost&&__arktosHost.bridgeUp"
            "&&window.__arktosShim)")).toBool(),
        15000);
    QCOMPARE(eval(offPage, QStringLiteral(
                 "self.__offBoot===true")).toBool(),
             true);
    offPage->runJavaScript(QStringLiteral(
        "self.__offIn={done:false};"
        "(async function(){"
        " await chrome.storage.local.set({offk:33});"
        " var g=await chrome.storage.local.get('offk');"
        " var h=await chrome.offscreen.hasDocument();"
        " self.__offIn={done:true,out:{stored:g&&g.offk===33,"
        "   selfHas:h,url:chrome.runtime.getURL('x')}};"
        "})().catch(function(e){"
        " self.__offIn={done:true,"
        "  fatal:String(e&&e.message||e)};});"));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(offPage, QStringLiteral(
            "!!(self.__offIn&&self.__offIn.done)")).toBool(),
        30000);
    const QJsonObject inDocWrap = evalJson(offPage, QStringLiteral(
        "JSON.stringify(self.__offIn)"));
    QVERIFY2(!inDocWrap.contains(QLatin1String("fatal")),
             qPrintable(QString::fromUtf8(
                 QJsonDocument(inDocWrap).toJson(
                     QJsonDocument::Compact))));
    const QJsonObject inDoc =
        inDocWrap.value(QLatin1String("out")).toObject();
    QCOMPARE(inDoc.value(QLatin1String("stored")).toBool(), true);
    QCOMPARE(inDoc.value(QLatin1String("selfHas")).toBool(), true);
    QVERIFY(inDoc.value(QLatin1String("url")).toString().startsWith(
        QStringLiteral("chrome-extension://") + id));

    // closeDocument releases the document; hasDocument/getContexts
    // turn honest again and the page is gone.
    host->page()->runJavaScript(QStringLiteral(
        "self.__offClose={done:false};"
        "(async function(){"
        " var o={};"
        " await chrome.offscreen.closeDocument();"
        " o.closed=true;"
        " o.hasAfterClose=await chrome.offscreen.hasDocument();"
        " var ctx=await chrome.runtime.getContexts({});"
        " o.stillListed=(ctx||[]).some(function(c){"
        "   return c.contextType==='OFFSCREEN_DOCUMENT';});"
        " try{await chrome.offscreen.closeDocument();"
        "  o.closeAgain='resolved';}"
        " catch(e){o.closeAgain=e.code||e.message;}"
        " self.__offClose={done:true,out:o};"
        "})().catch(function(e){"
        " self.__offClose={done:true,"
        "  fatal:String(e&&e.message||e)};});"));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(host->page(), QStringLiteral(
            "!!(self.__offClose&&self.__offClose.done)")).toBool(),
        30000);
    const QJsonObject closed = evalJson(host->page(), QStringLiteral(
        "JSON.stringify(self.__offClose)"));
    QVERIFY2(!closed.contains(QLatin1String("fatal")),
             qPrintable(QString::fromUtf8(
                 QJsonDocument(closed).toJson(
                     QJsonDocument::Compact))));
    const QJsonObject cout =
        closed.value(QLatin1String("out")).toObject();
    QCOMPARE(cout.value(QLatin1String("closed")).toBool(), true);
    QCOMPARE(cout.value(QLatin1String("hasAfterClose")).toBool(),
             false);
    QCOMPARE(cout.value(QLatin1String("stillListed")).toBool(),
             false);
    QCOMPARE(cout.value(QLatin1String("closeAgain")).toString(),
             QStringLiteral("no-document"));
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(!runtime->offscreenHasDocument(id));
    QVERIFY(!runtime->offscreenPage(id));

    // No 'offscreen' permission → the whole namespace answers with
    // the typed denial, never silently.
    manager->grantConsent(noPermDir);
    manager->loadExtension(noPermDir);
    loadTimer.restart();
    while (loadedNp->isEmpty() && loadTimer.elapsed() < 30000)
        QTest::qWait(50);
    const QString npId = *loadedNp;
    QVERIFY2(!npId.isEmpty(), "no-permission fixture must install");
    manager->setExtensionEnabled(npId, true);
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime->host(npId)
            && runtime->host(npId)->state()
                   == ExtensionBackgroundHost::State::Ready,
        45000);
    runtime->host(npId)->page()->runJavaScript(QStringLiteral(
        "self.__offNp={done:false};"
        "(async function(){"
        " var o={};"
        " try{await chrome.offscreen.hasDocument();"
        "  o.has='resolved';}catch(e){o.has=e.code||e.message;}"
        " try{await chrome.offscreen.createDocument({url:'off.html'"
        "  ,reasons:['TESTING'],justification:'x'});"
        "  o.create='resolved';}catch(e){o.create=e.code||e.message;}"
        " self.__offNp={done:true,out:o};"
        "})().catch(function(e){"
        " self.__offNp={done:true,"
        "  fatal:String(e&&e.message||e)};});"));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(runtime->host(npId)->page(), QStringLiteral(
            "!!(self.__offNp&&self.__offNp.done)")).toBool(),
        30000);
    const QJsonObject denied = evalJson(
        runtime->host(npId)->page(), QStringLiteral(
            "JSON.stringify(self.__offNp.out||{})"));
    QCOMPARE(denied.value(QLatin1String("has")).toString(),
             QStringLiteral("no-permission"));
    QCOMPARE(denied.value(QLatin1String("create")).toString(),
             QStringLiteral("no-permission"));

    // --- teardown -----------------------------------------------------
    manager->setExtensionEnabled(npId, false);
    QTRY_VERIFY_WITH_TIMEOUT(!runtime->host(npId), 20000);
    manager->removeExtension(npId);
    manager->setExtensionEnabled(id, false);
    QTRY_VERIFY_WITH_TIMEOUT(!runtime->host(id), 20000);
    manager->removeExtension(id);
    QTest::qWait(500);
    QDir(root).removeRecursively();
}

// EXT20: the engine forgets isEnabled across restarts — installed
// extensions enumerate disabled on a cold profile and the action
// popup then renders without chrome.runtime (dead-white).  Arktos
// persists the choice under extensions/enabled/<storage>/<id> and
// re-applies it on engine load, so the flag must follow the toggle
// and uninstall must clear it.
void tst_ExtensionManager::enabledStatePersists()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");

    const QString root = QDir::temp().filePath(
        QStringLiteral("tst-extena-%1")
            .arg(QCoreApplication::applicationPid()));
    const QString dir = writeExtension(root, QStringLiteral("tst-ena"),
                                       QStringLiteral("1.0.0"));
    QVERIFY(!dir.isEmpty());

    ExtensionManager *manager = ExtensionManager::instance();
    QWebEngineProfile *profile = BrowserApplication::webEngineProfile();

    for (const ExtensionManager::ExtensionInfo &info
         : manager->extensions()) {
        if (!info.builtin && info.name == QLatin1String("tst-ena"))
            manager->removeExtension(info.id);
    }
    QTest::qWait(300);

    const QString id = installFixture(dir);
    QVERIFY2(!id.isEmpty(), "fixture extension must install");

    const QString storage = profile->storageName().isEmpty()
        ? QStringLiteral("default") : profile->storageName();
    const QString key = QStringLiteral("extensions/enabled/")
        + storage + QLatin1Char('/') + id;

    manager->setExtensionEnabled(id, true);
    QCOMPARE(ProfileSettings().value(key).toBool(), true);
    manager->setExtensionEnabled(id, false);
    QCOMPARE(ProfileSettings().value(key).toBool(), false);

    manager->removeExtension(id);
    QTRY_VERIFY_WITH_TIMEOUT(
        !ProfileSettings().contains(key), 10000);

    QDir(root).removeRecursively();
}

QTEST_MAIN(tst_ExtensionManager)
#include "tst_extensionmanager.moc"
