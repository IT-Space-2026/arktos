/*
 * Copyright 2026 Arktos Browser - IT-Space
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
#include <qwebenginesettings.h>

#include <memory>

#include <qcheckbox.h>
#include <qevent.h>
#include <qlistwidget.h>
#include <qpushbutton.h>

#include "bookmarksmanager.h"
#include "bookmarknode.h"
#include "browserapplication.h"
#include "browsermainwindow.h"
#include "browserpaths.h"
#include "browserprofile.h"
#include "cookiejar.h"
#include "downloadmanager.h"
#include "extensionactionbar.h"
#include "extensionbackgroundhost.h"
#include "extensioncommands.h"
#include "extensioneventhub.h"
#include "extensionidlewatcher.h"
#include "extensionmanager.h"
#include "extensionnotificationcenter.h"
#include "extensionpagebridge.h"
#include "extensionrequestwatcher.h"
#include "extensionreviewdialog.h"
#include "extensionshimbackend.h"
#include "historymanager.h"
#include "engineinterface.h"
#include "opensearchmanager.h"
#include "profilesettings.h"
#include "qtest_arktos.h"
#include "tabwidget.h"
#include "toolbarsearch.h"
#include "webpage.h"
#include "webview.h"

#include <qframe.h>
#include <qmenu.h>
#include <qsignalspy.h>
#include <qwebenginecookiestore.h>
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
    void runtimeHealthRegrade();
    void backgroundHealthProbe();
    void hostedBackgroundRuntime();
    void hostedHostChurn();
    void chromePageChurn();
    void relayPortChurn();
    void extensionShimBackend();
    void extensionStorageExtras();
    void extensionAlarmEdges();
    void extensionTabsOps();
    void extensionTabsZoomCapture();
    void extensionMessageRelay();
    void extensionChromeNavLock();
    void extensionEventSurfaces();
    void extensionWebRequest();
    void extensionRuntimeLifecycle();
    void extensionPopupContext();
    void extensionPageAgent();
    void extensionTabMessaging();
    void extensionTabConnect();
    void extensionWindowsFidelity();
    void extensionActionIcon();
    void extensionAllowedAccess();
    void extensionManagementRead();
    void extensionManagementMutations();
    void extensionBookmarks();
    void extensionHistory();
    void extensionCookies();
    void extensionDownloads();
    void extensionSessions();
    void extensionTabGroups();
    void extensionTopSites();
    void extensionBrowsingData();
    void extensionDns();
    void extensionSystem();
    void extensionPrivacy();
    void extensionRuntimeOps();
    void extensionScripting();
    void extensionScriptCss();
    void extensionContentScriptRegistration();
    void extensionUserScripts();
    void extensionNotifications();
    void extensionIdle();
    void extensionCommands();
    void extensionOffscreen();
    void extensionIntrospection();
    void actionPopupSelfWires();
    void realExtensionPopup();
    void bitwardenGapBaseline();
    void bitwardenHandshakeGate();
    void bitwardenUnlockFill();
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

    // ER12: count a backend's pushEvent emissions carrying `name`.
    static int pushCount(const QSignalSpy &spy, const QString &name) {
        int n = 0;
        for (const QList<QVariant> &args : spy) {
            if (!args.isEmpty()
                    && args.at(0).toString() == name)
                ++n;
        }
        return n;
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

    // ER18 — the residual sweep's contract.  .devin/EXT-RUNTIME-GAPS.md
    // is the lane's declared intentional set: a ```gaps fenced block
    // of signatures, `chrome.<api>: <code>` exact, or
    // `chrome.<ns>.*: <code>` covering every member of a wholly
    // unserved namespace.  A typed gap the bundle emits that is not
    // listed there is an unplanned regression.  ARKTOS_EXTGAPS_DOC
    // overrides the lookup; otherwise the file is found by walking
    // up from the test binary's directory.
    static QStringList documentedGaps() {
        QString path = qEnvironmentVariable("ARKTOS_EXTGAPS_DOC");
        if (path.isEmpty()) {
            QDir dir(QCoreApplication::applicationDirPath());
            for (int i = 0; i < 8; ++i) {
                const QString cand = dir.absoluteFilePath(
                    QStringLiteral(".devin/EXT-RUNTIME-GAPS.md"));
                if (QFile::exists(cand)) {
                    path = cand;
                    break;
                }
                if (!dir.cdUp())
                    break;
            }
        }
        QStringList allowed;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return allowed;
        bool inBlock = false;
        while (!file.atEnd()) {
            const QString line =
                QString::fromUtf8(file.readLine()).trimmed();
            if (line.startsWith(QLatin1String("```"))) {
                if (inBlock)
                    inBlock = false;
                else
                    inBlock = (line == QLatin1String("```gaps"));
                continue;
            }
            if (inBlock && !line.isEmpty()
                    && !line.startsWith(QLatin1Char('#')))
                allowed.append(line);
        }
        return allowed;
    }

    static bool gapDocumented(const QStringList &allowed,
                              const QString &gap) {
        for (const QString &a : allowed) {
            const int star = a.indexOf(QLatin1String(".*:"));
            if (star >= 0) {
                // 'chrome.<ns>.*: <code>' — namespace prefix plus the
                // trailing ': <code>' must both hold.
                if (gap.startsWith(a.left(star + 1))
                        && gap.endsWith(a.mid(star + 2)))
                    return true;
            } else if (gap == a) {
                return true;
            }
        }
        return false;
    }

    static QStringList undocumentedGaps(const QStringList &gaps) {
        const QStringList allowed = documentedGaps();
        QStringList out;
        for (const QString &g : gaps)
            if (!g.isEmpty() && !gapDocumented(allowed, g))
                out.append(g);
        return out;
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
                      "\"permissions\":[\"gcm\",\"webRequest\","
                      "\"cookies\",\"nativeMessaging\"],"
                      "\"host_permissions\":[\"<all_urls>\","
                      "\"*://*.example.com/*\"]}")
        << true << 3 << QStringLiteral("perms") << false
        << (QStringList() << QStringLiteral("gcm"))
        << (QStringList() << QStringLiteral("webRequest"))
        // ER19: webRequest is runtime-served — it leaves the fatal
        // set (and stays in 'unverified' for honest wording).
        << QStringList()
        << (QStringList() << QStringLiteral("cookies")
                          << QStringLiteral("nativeMessaging"))
        << (QStringList() << QStringLiteral("<all_urls>"));
    // EXT21+ER19: required scripting marked the extension's purpose
    // dead — the Bitwarden signature.  The hosted runtime serves it
    // now, so it leaves the fatal set; an OPTIONAL scripting grant
    // still must not condemn anything either.
    QTest::newRow("served core api required")
        << QByteArray("{\"manifest_version\":3,\"name\":\"vault\","
                      "\"version\":\"1.0\","
                      "\"permissions\":[\"scripting\",\"storage\"]}")
        << true << 3 << QStringLiteral("vault") << false
        << QStringList()
        << (QStringList() << QStringLiteral("scripting"))
        << QStringList()
        << QStringList() << QStringList();
    // A required fatal-table permission the runtime does NOT serve
    // still condemns the extension — the regrade subtracts only the
    // served set.
    QTest::newRow("unserved fatal api required")
        << QByteArray("{\"manifest_version\":3,\"name\":\"dnr\","
                      "\"version\":\"1.0\","
                      "\"permissions\":[\"declarativeNetRequest\","
                      "\"storage\"]}")
        << true << 3 << QStringLiteral("dnr") << false
        << QStringList()
        << (QStringList() << QStringLiteral("declarativeNetRequest"))
        << (QStringList() << QStringLiteral("declarativeNetRequest"))
        << QStringList() << QStringList();
    // ER19: runtime-served 'unsupported' members migrate to
    // 'unverified' — the honest tier is "partially supported", not
    // "will not work".  MD23 serves 'cookies' (it migrates too);
    // 'gcm' is genuinely dead and stays.
    QTest::newRow("runtime-served apis migrate")
        << QByteArray("{\"manifest_version\":3,\"name\":\"served\","
                      "\"version\":\"1.0\","
                      "\"permissions\":[\"notifications\",\"tabs\","
                      "\"bookmarks\",\"cookies\",\"topSites\",\"gcm\"]}")
        << true << 3 << QStringLiteral("served") << false
        << (QStringList() << QStringLiteral("gcm"))
        << (QStringList() << QStringLiteral("notifications")
                          << QStringLiteral("tabs")
                          << QStringLiteral("bookmarks")
                          << QStringLiteral("cookies")
                          << QStringLiteral("topSites"))
        << QStringList()
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

    // ER19: the manifest verdict is runtime-aware — pin the armed
    // runtime so these rows assert the regraded shape; the raw
    // engine-verdict path is covered by runtimeHealthRegrade().
    const QByteArray savedExthost = qgetenv("ARKTOS_EXTHOST_OFF");
    qunsetenv("ARKTOS_EXTHOST_OFF");

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
    if (!savedExthost.isEmpty())
        qputenv("ARKTOS_EXTHOST_OFF", savedExthost);
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

// ER19 — the health regrade the EXT21 fatal table was waiting for.
// With the hosted runtime armed, a required permission the runtime
// serves (scripting, webRequest, the event-stream namespaces) leaves
// the fatal set and migrates out of 'unsupported' into 'unverified'
// — "partially supported" is the honest tier, never "will not work".
// What the runtime does NOT serve keeps the raw engine verdict: a
// required declarativeNetRequest still condemns the extension, and
// ARKTOS_EXTHOST_OFF reverses the whole regrade — a fixture
// requiring scripting without the runtime path still grades
// NotSupported.
void tst_ExtensionManager::runtimeHealthRegrade()
{
    using EH = ExtensionManager::ExtensionHealth;
    using PV = ExtensionManager::ProbeVerdict;

    const QString root = QDir::temp().filePath(
        QStringLiteral("tst-extregrade-%1")
            .arg(QCoreApplication::applicationPid()));
    const QString dir = writeExtension(
        root, QStringLiteral("tst-regrade"), QStringLiteral("1.0.0"),
        QString(), {QStringLiteral("scripting"),
                    QStringLiteral("notifications"),
                    QStringLiteral("cookies"),
                    QStringLiteral("gcm")});
    QVERIFY(!dir.isEmpty());

    // Runtime armed (the suite's normal env — pin it so a stray
    // ARKTOS_EXTHOST_OFF in the caller's environment cannot skew the
    // regrade).
    const QByteArray savedExthost = qgetenv("ARKTOS_EXTHOST_OFF");
    qunsetenv("ARKTOS_EXTHOST_OFF");

    const ExtensionManager::Manifest served =
        ExtensionManager::inspectManifest(dir);
    QVERIFY(served.valid);
    // scripting leaves fatalMissing and was always 'unverified';
    // notifications migrates unsupported -> unverified, and MD23's
    // cookies service moves it the same way; gcm is genuinely dead
    // and stays the missingApis marker.
    QVERIFY(served.fatalMissing.isEmpty());
    QVERIFY(served.unverified.contains(QLatin1String("scripting")));
    QVERIFY(served.unverified.contains(QLatin1String("notifications")));
    QVERIFY(served.unverified.contains(QLatin1String("cookies")));
    QVERIFY(!served.unsupported.contains(QLatin1String("notifications")));
    QVERIFY(!served.unsupported.contains(QLatin1String("cookies")));
    QVERIFY(served.unsupported.contains(QLatin1String("gcm")));
    // Regraded: the same extension that EXT21 condemned is
    // Limited-or-better — its remaining gaps are honest, not fatal.
    QCOMPARE(ExtensionManager::classifyHealth(
                 true, served.unsupported, served.fatalMissing,
                 PV::Responsive),
             EH::Limited);
    QCOMPARE(ExtensionManager::classifyHealth(
                 true, served.unsupported, served.fatalMissing,
                 PV::Dead),
             EH::NotSupported);

    // A required fatal-table member the runtime does NOT serve still
    // condemns the extension even with the runtime armed.
    const QString dnrDir = writeExtension(
        root, QStringLiteral("tst-regrade-dnr"), QStringLiteral("1.0.0"),
        QString(), {QStringLiteral("declarativeNetRequest"),
                    QStringLiteral("scripting")});
    const ExtensionManager::Manifest dnr =
        ExtensionManager::inspectManifest(dnrDir);
    QVERIFY(!dnr.fatalMissing.contains(QLatin1String("scripting")));
    QVERIFY(dnr.fatalMissing.contains(
                QLatin1String("declarativeNetRequest")));
    QCOMPARE(ExtensionManager::classifyHealth(
                 true, dnr.unsupported, dnr.fatalMissing,
                 PV::Responsive),
             EH::NotSupported);

    // Without the runtime path the raw engine verdict stands —
    // scripting is fatal again and served surfaces stay "won't
    // work".  (inspectManifest reads the env per call, so toggling
    // here is the honest "no runtime" model.)
    qputenv("ARKTOS_EXTHOST_OFF", "1");
    const ExtensionManager::Manifest raw =
        ExtensionManager::inspectManifest(dir);
    if (!savedExthost.isEmpty())
        qputenv("ARKTOS_EXTHOST_OFF", savedExthost);
    else
        qunsetenv("ARKTOS_EXTHOST_OFF");
    QVERIFY(raw.fatalMissing.contains(QLatin1String("scripting")));
    QVERIFY(raw.unsupported.contains(QLatin1String("notifications")));
    QVERIFY(!raw.unverified.contains(QLatin1String("notifications")));
    QCOMPARE(ExtensionManager::classifyHealth(
                 true, raw.unsupported, raw.fatalMissing,
                 PV::Responsive),
             EH::NotSupported);

    // The runtimeServes() list covers the fatal members it rescues
    // and only them — never the still-dead request APIs.
    const QStringList serves = ExtensionManager::runtimeServes();
    QVERIFY(serves.contains(QLatin1String("scripting")));
    QVERIFY(serves.contains(QLatin1String("webRequest")));
    QVERIFY(!serves.contains(QLatin1String("declarativeNetRequest")));
    QVERIFY(!serves.contains(QLatin1String("webRequestBlocking")));

    QDir(root).removeRecursively();
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
    // Bitwarden's failure mode (Chromium status 15).  'gcm' is the
    // unsupported-permission marker: the hosted runtime does not
    // serve it (ER19), so it stays in missingApis either way.
    const QString deadDir = writeExtension(
        root, QStringLiteral("tst-health-dead"),
        QStringLiteral("1.0.0"), QString(),
        {QStringLiteral("gcm")});
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
    // "gcm" is unsupported even with the hosted runtime armed
    // — the reason names the missing surface, not just the crash.
    QVERIFY(dead.missingApis.contains(QLatin1String("gcm")));

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
            "\"version\":\"1.0\",\"permissions\":[\"storage\","
            "\"alarms\"],"
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
        // MD14: setIcon rides the real bridge — imageData crosses as
        // base64'd raw RGBA, a size->path map lands verbatim on the
        // backend (and replaces the image override, Chrome parity).
        " await chrome.action.setIcon({imageData:{width:2,height:2,"
        "   data:new Uint8ClampedArray([255,0,0,255,0,255,0,255,"
        "   0,0,255,255,255,255,255,255])}});"
        " out.iconImage=true;"
        " await chrome.browserAction.setIcon("
        "   {path:{'16':'icons/a.png'}});"
        " out.iconPath=true;"
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
             QStringLiteral("no-permission"));
    QVERIFY(out.value(QLatin1String("msgCallback")).toBool());

    // Action state flows into the C++ registry EXT08's button strip
    // reads; the fixture lacks 'notifications' so create's typed
    // no-permission denial reached the host's gap list instead of
    // silently no-oping.
    const ExtensionShimBackend::ActionState action =
        ExtensionShimBackend::actionState(id);
    QCOMPARE(action.title, QStringLiteral("Shim Title"));
    QCOMPARE(action.badgeText, QStringLiteral("7"));
    // Both setIcon spellings resolved; the path call is the recorded
    // override now (it replaced the imageData one).
    QVERIFY(out.value(QLatin1String("iconImage")).toBool());
    QVERIFY(out.value(QLatin1String("iconPath")).toBool());
    QCOMPARE(action.iconPaths.value(16),
             QStringLiteral("icons/a.png"));
    QVERIFY(action.iconImages.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime->host(id)->shimGaps().contains(
            QStringLiteral("chrome.notifications.create: no-permission")),
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

// MEM11 — enable/disable churn for the hosted runtime: every cycle
// mints a full ExtensionBackgroundHost (hidden view + page +
// channel + bridge + timers) and disable must tear ALL of it down —
// the leak sweep's in-test half is that each host object really
// dies (a retained one fails here AND in the LSan report).
void tst_ExtensionManager::hostedHostChurn()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");

    const QString dir = QDir::temp().filePath(
        QStringLiteral("tst-extchurn-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dir));
    {
        QFile manifestFile(dir + QLatin1String("/manifest.json"));
        QVERIFY(manifestFile.open(QIODevice::WriteOnly));
        manifestFile.write(
            "{\"manifest_version\":3,\"name\":\"tst-churn\","
            "\"version\":\"1.0\",\"permissions\":[\"storage\"],"
            "\"background\":{\"service_worker\":\"sw.js\"}}");
        manifestFile.close();
        QFile worker(dir + QLatin1String("/sw.js"));
        QVERIFY(worker.open(QIODevice::WriteOnly));
        worker.write(
            "chrome.runtime.onInstalled.addListener(function(){});\n");
        worker.close();
    }

    ExtensionManager *manager = ExtensionManager::instance();
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();

    auto loaded = std::make_shared<QString>();
    QObject::connect(manager, &ExtensionManager::extensionLoaded,
                     this, [loaded](
                         const ExtensionManager::ExtensionInfo &info) {
        if (info.name == QLatin1String("tst-churn")
                && info.error.isEmpty())
            *loaded = info.id;
    });
    manager->grantConsent(dir);
    manager->loadExtension(dir);
    QTRY_VERIFY_WITH_TIMEOUT(!loaded->isEmpty(), 30000);
    const QString id = *loaded;

    for (int cycle = 0; cycle < 3; ++cycle) {
        manager->setExtensionEnabled(id, true);
        QTRY_VERIFY_WITH_TIMEOUT(
            runtime->host(id)
                && runtime->host(id)->state()
                       == ExtensionBackgroundHost::State::Ready,
            45000);
        ExtensionBackgroundHost *host = runtime->host(id);
        QPointer<ExtensionBackgroundHost> hostGuard(host);
        QPointer<QWebEngineView> viewGuard(host->view());
        QPointer<QWebEnginePage> pageGuard(host->page());
        QPointer<ChromeBridge> bridgeGuard(host->bridge());

        manager->setExtensionEnabled(id, false);
        QTRY_VERIFY_WITH_TIMEOUT(!runtime->host(id), 15000);
        // disable's teardown is deleteLater — the QTRY above pumped
        // the deferred deletes already.
        QTRY_VERIFY_WITH_TIMEOUT(
            !hostGuard && !viewGuard && !pageGuard && !bridgeGuard,
            15000);
    }

    manager->removeExtension(id);
    QTRY_VERIFY_WITH_TIMEOUT(!runtime->host(id), 15000);
    QDir(dir).removeRecursively();
}

// MRG07 — the merge's newest allocation path under churn: the
// ExtensionChromePage surfaces (action popup + health probe).  Every
// popup open mints a Qt::Popup frame + view + nav-locked page with the
// ER20 JavascriptEnabled lift, and every forced probe mints its own
// parentless ExtensionChromePage — dismiss/finish must tear ALL of it
// down.  The QPointer guards are the in-test half of the leak sweep:
// a retained page fails here AND in the LSan report.
void tst_ExtensionManager::chromePageChurn()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");

    const QString dir = QDir::temp().filePath(
        QStringLiteral("tst-chromechurn-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dir));
    {
        QFile manifestFile(dir + QLatin1String("/manifest.json"));
        QVERIFY(manifestFile.open(QIODevice::WriteOnly));
        manifestFile.write(
            "{\"manifest_version\":3,\"name\":\"tst-chromechurn\","
            "\"version\":\"1.0\",\"permissions\":[\"storage\"],"
            "\"action\":{\"default_popup\":\"popup.html\"},"
            "\"background\":{\"service_worker\":\"sw.js\"}}");
        manifestFile.close();
        QFile worker(dir + QLatin1String("/sw.js"));
        QVERIFY(worker.open(QIODevice::WriteOnly));
        worker.write(
            "chrome.runtime.onInstalled.addListener(function(){});\n");
        worker.close();
        QFile html(dir + QLatin1String("/popup.html"));
        QVERIFY(html.open(QIODevice::WriteOnly));
        html.write("<!doctype html><html><body>churn</body></html>");
        html.close();
    }

    ExtensionManager *manager = ExtensionManager::instance();
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();

    auto loaded = std::make_shared<QString>();
    QObject::connect(manager, &ExtensionManager::extensionLoaded,
                     this, [loaded](
                         const ExtensionManager::ExtensionInfo &info) {
        if (info.name == QLatin1String("tst-chromechurn")
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

    // --- action popup open/dismiss cycles --------------------------
    ExtensionActionBar bar;
    bar.show();
    for (int cycle = 0; cycle < 3; ++cycle) {
        QVERIFY(bar.triggerAction(id, QStringLiteral("popup.html")));
        QFrame *popup = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT(
            (popup = bar.findChild<QFrame *>(
                 QStringLiteral("extensionActionPopup"))) != nullptr,
            10000);
        QWebEngineView *view = popup->findChild<QWebEngineView *>();
        QVERIFY(view);
        ExtensionChromePage *page =
            qobject_cast<ExtensionChromePage *>(view->page());
        QVERIFY2(page,
                 "action popup must carry the nav-locked "
                 "ExtensionChromePage, not a plain QWebEnginePage");
        // MRG10/SEC26-1: BOTH halves of the fix must be live on the
        // real chrome page — the navigation lock refuses a foreign
        // url AND the ER20 JavascriptEnabled lift is on.
        QVERIFY(!ExtensionChromePage::navigationAllowed(
                    QUrl(QStringLiteral("https://forged.example/x")),
                    id, true));
        QVERIFY(page->settings()->testAttribute(
                    QWebEngineSettings::JavascriptEnabled));
        QSignalSpy loadSpy(page, &QWebEnginePage::loadFinished);
        const QUrl popupUrl(QStringLiteral("chrome-extension://") + id
                            + QStringLiteral("/popup.html"));
        // loadFinished may already have fired before the spy existed
        // — a committed popup url is the same proof.
        QTRY_VERIFY_WITH_TIMEOUT(
            loadSpy.count() >= 1 || page->url() == popupUrl, 30000);
        const QPointer<QFrame> popupGuard(popup);
        const QPointer<QWebEngineView> viewGuard(view);
        const QPointer<QWebEnginePage> pageGuard(page);
        popup->close();
        QTRY_VERIFY_WITH_TIMEOUT(
            !popupGuard && !viewGuard && !pageGuard, 15000);
    }

    // --- health-probe mint/destroy cycles --------------------------
    // force re-measures: each pass deletes the prior verdict, mints a
    // fresh probe page, runs the ping, and finishHealthProbe deletes
    // it.  The guard asserts every minted page dies.
    for (int cycle = 0; cycle < 3; ++cycle) {
        manager->requestHealthProbe(id, true);
        QWebEnginePage *probe = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT(
            (probe = qobject_cast<ExtensionChromePage *>(
                 manager->healthProbePage())) != nullptr,
            15000);
        // MRG10/SEC26-1: same both-halves check on the probe page —
        // nav-locked to the extension origin AND carrying the ER20
        // JavascriptEnabled lift.
        QVERIFY(!ExtensionChromePage::navigationAllowed(
                    QUrl(QStringLiteral("https://forged.example/x")),
                    id, true));
        QVERIFY(probe->settings()->testAttribute(
                    QWebEngineSettings::JavascriptEnabled));
        const QPointer<QWebEnginePage> probeGuard(probe);
        QTRY_VERIFY_WITH_TIMEOUT(
            !manager->healthProbeDetail(id).isEmpty(), 60000);
        QTRY_VERIFY_WITH_TIMEOUT(!probeGuard, 15000);
    }

    manager->setExtensionEnabled(id, false);
    QTRY_VERIFY_WITH_TIMEOUT(!runtime->host(id), 20000);
    manager->removeExtension(id);
    QDir(dir).removeRecursively();
}

// MRG07 — PortEnd + pending-mid churn, the second merge seam: the
// relay's register/expire cycles exercised AT and PAST the SEC26-3
// bounds (kMaxPortEndsPerChan = 64, kMaxPortChans = 1024,
// kMaxRelayPending = 512), on BOTH kinds merged master's generalized
// PortEnd::target carries — ChromeBridge shim contexts and
// ExtensionPageBridge page agents — plus every removal path
// (respond/disconnect, the 30 s expiry timers, unregister death).
void tst_ExtensionManager::relayPortChurn()
{
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();

    const QString dir = QDir::temp().filePath(
        QStringLiteral("tst-relaychurn-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dir));
    {
        QFile f(dir + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-relaychurn\","
                "\"version\":\"1.0\"}");
        f.close();
    }
    const QString extId = QStringLiteral("relaychurn00000000000001");
    ChromeBridge bg(extId, QStringLiteral("tst-relaychurn"), dir);
    ChromeBridge peer(extId, QStringLiteral("tst-relaychurn"), dir);
    QVERIFY(!runtime->registerRelayContext(
                &bg, QStringLiteral("background")).isEmpty());
    QVERIFY(!runtime->registerRelayContext(
                &peer, QStringLiteral("popup")).isEmpty());

    // --- per-chan end cap, at and past ------------------------------
    const QString capChan = extId + QStringLiteral("#cap");
    for (int i = 0; i < 64; ++i)
        QCOMPARE(runtime->relayPortConnect(
                     &bg, capChan, QStringLiteral("e%1").arg(i),
                     QString(), QString()), 1);
    // End 65 is refused — no append, no fan-out.
    QCOMPARE(runtime->relayPortConnect(
                 &bg, capChan, QStringLiteral("e64"),
                 QString(), QString()), 0);
    // Dedup under the cap: re-registering e0 refuses too.
    QCOMPARE(runtime->relayPortConnect(
                 &bg, capChan, QStringLiteral("e0"),
                 QString(), QString()), 0);
    // The 63 non-sender ends still deliver.
    QCOMPARE(runtime->relayPortMessage(
                 &bg, capChan, QStringLiteral("e0"),
                 QJsonValue(QStringLiteral("m"))), 63);
    // Expire half: disconnect every end — the chan dies on empty.
    for (int i = 0; i < 64; ++i)
        runtime->relayPortDisconnect(
            &bg, capChan, QStringLiteral("e%1").arg(i));
    QCOMPARE(runtime->relayPortMessage(
                 &bg, capChan, QStringLiteral("e0"),
                 QJsonValue(1)), 0);

    // --- channel-count cap, at and past -----------------------------
    // The check runs AFTER m_portEnds[chan] inserts the empty list,
    // so the bound is map-size 1024 = at most 1023 populated chans —
    // the 1024th distinct new channel is the first refusal.
    int accepted = 0;
    for (int i = 0; i < 1100; ++i) {
        if (runtime->relayPortConnect(
                &bg, extId + QStringLiteral("#c%1").arg(i),
                QStringLiteral("x"), QString(), QString()) == 1)
            ++accepted;
        else
            break;
    }
    QCOMPARE(accepted, 1023);
    // An existing chan still accepts ends under its per-chan cap.
    QCOMPARE(runtime->relayPortConnect(
                 &bg, extId + QStringLiteral("#c0"),
                 QStringLiteral("y"), QString(), QString()), 1);
    QCOMPARE(runtime->relayPortMessage(
                 &bg, extId + QStringLiteral("#c0"),
                 QStringLiteral("x"), QJsonValue(1)), 1);
    // Disconnect a slice; the rest die wholesale on the unregister
    // below — the death-path sweep of the same ends.
    for (int i = 1; i < 16; ++i)
        runtime->relayPortDisconnect(
            &bg, extId + QStringLiteral("#c%1").arg(i),
            QStringLiteral("x"));

    // --- pending-mid churn ------------------------------------------
    // Resolved cycles: send -> respond clears entry AND its timer.
    for (int cycle = 0; cycle < 4; ++cycle) {
        const QString mid = extId + QStringLiteral("#m%1").arg(cycle);
        QCOMPARE(runtime->relaySend(&bg, mid, QJsonObject()), 1);
        QVERIFY(runtime->relayRespond(
                    &peer, mid, QJsonValue(cycle)));
    }
    // Fill the pending bound; the send past it is refused.
    for (int i = 0; i < 512; ++i) {
        const QString mid = extId + QStringLiteral("#f%1").arg(i);
        QCOMPARE(runtime->relaySend(&bg, mid, QJsonObject()), 1);
    }
    QCOMPARE(runtime->relaySend(
                 &bg, extId + QStringLiteral("#f512"),
                 QJsonObject()), 0);
    // Death-path expire: unregister removes every pending mid this
    // bridge owns AND its expiry timer (and sweeps the ~1000 live
    // port-end channels above, pushing disconnects at survivors).
    runtime->unregisterRelayContext(&bg);
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(!runtime->relayRespond(
                &peer, extId + QStringLiteral("#f0"),
                QJsonValue(1)));
    QVERIFY(!runtime->registerRelayContext(
                &bg, QStringLiteral("background")).isEmpty());

    // --- page-agent ends: the OTHER PortEnd::target kind ------------
    auto *view = new QWebEngineView;
    auto *page = new QWebEnginePage(
        BrowserApplication::instance()->webEngineProfile(), view);
    view->setPage(page);
    page->load(QUrl(QStringLiteral("about:blank")));
    QTRY_VERIFY_WITH_TIMEOUT(
        page->url() == QUrl(QStringLiteral("about:blank")), 15000);
    auto *agent = new ExtensionPageBridge(page);
    QSignalSpy agentSpy(agent, &ExtensionPageBridge::extEvent);
    QSignalSpy bgSpy(&bg, &ChromeBridge::hostEvent);

    // MRG10/SEC26-3 merge parity: page-agent ends share the channel-
    // count bound with bridge ends — the same 1023-populated
    // boundary the bridge leg above observed, enforced while
    // PortEnd::target carries an ExtensionPageBridge.
    int pageChans = 0;
    for (int i = 0; i < 1100; ++i) {
        if (runtime->pagePortConnect(
                agent, extId,
                extId + QStringLiteral("#pc%1").arg(i),
                QStringLiteral("x"), QString(), QJsonObject())
                .value(QLatin1String("ok")).toBool())
            ++pageChans;
        else
            break;
    }
    QCOMPARE(pageChans, 1023);
    QCOMPARE(runtime->pagePortConnect(
                 agent, extId, extId + QStringLiteral("#pcx"),
                 QStringLiteral("x"), QString(), QJsonObject())
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("too-many-ends"));
    // The bound is kind-agnostic — a bridge connect on a fresh chan
    // is refused while the map is full of page ends.
    QCOMPARE(runtime->relayPortConnect(
                 &bg, extId + QStringLiteral("#pgfull"),
                 QStringLiteral("x"), QString(), QString()), 0);
    // Drain the fill so the live-chan assertions below start clean.
    for (int i = 0; i < pageChans; ++i)
        runtime->pagePortDisconnect(
            agent, extId + QStringLiteral("#pc%1").arg(i),
            QStringLiteral("x"));

    // Page + bridge ends share one chan — deliveries cross kinds.
    const QString pageChan = extId + QStringLiteral("#pg");
    QVERIFY(runtime->pagePortConnect(
                agent, extId, pageChan, QStringLiteral("pg0"),
                QStringLiteral("n"), QJsonObject())
                .value(QLatin1String("ok")).toBool());
    QCOMPARE(runtime->relayPortConnect(
                 &bg, pageChan, QStringLiteral("bg0"),
                 QString(), QString()), 1);
    // page -> bridge end.
    QCOMPARE(runtime->pagePortMessage(
                 agent, pageChan, QStringLiteral("pg0"),
                 QJsonValue(QStringLiteral("hi")))
                 .value(QLatin1String("result")).toObject()
                 .value(QLatin1String("delivered")).toInt(),
             1);
    // bridge -> page end: '__arktos.portMessage' lands on extEvent.
    QCOMPARE(runtime->relayPortMessage(
                 &bg, pageChan, QStringLiteral("bg0"),
                 QJsonValue(QStringLiteral("yo"))), 1);
    QTRY_VERIFY_WITH_TIMEOUT(agentSpy.size() >= 1, 10000);
    QCOMPARE(agentSpy.last().at(0).toString(),
             QStringLiteral("__arktos.portMessage"));

    // MRG10/SEC26-3: page-end dedup — re-firing a connect for an
    // already-registered (page, portId) pair is a no-op like the
    // bridge-side leg above: no second end, no re-fan-out.
    const int bgEventsBefore = bgSpy.size();
    QVERIFY(runtime->pagePortConnect(
                agent, extId, pageChan, QStringLiteral("pg0"),
                QStringLiteral("n"), QJsonObject())
                .value(QLatin1String("ok")).toBool());
    QCOMPARE(bgSpy.size(), bgEventsBefore);
    // One end means one delivery — a duplicated registration would
    // double-count on the same (target, portId) pair.
    QCOMPARE(runtime->relayPortMessage(
                 &bg, pageChan, QStringLiteral("bg0"),
                 QJsonValue(QStringLiteral("dupchk"))), 1);
    // pagePortAccept dedups the same way.
    QVERIFY(runtime->pagePortAccept(
                agent, pageChan, QStringLiteral("pg0"))
                .value(QLatin1String("ok")).toBool());
    QCOMPARE(runtime->relayPortMessage(
                 &bg, pageChan, QStringLiteral("bg0"),
                 QJsonValue(QStringLiteral("dupchk2"))), 1);

    // MRG10/SEC26-3: the per-chan bound counts BOTH PortEnd kinds —
    // bridge ends and page ends fill the same 64-end list.
    const QString mixChan = extId + QStringLiteral("#mixcap");
    for (int i = 0; i < 63; ++i)
        QCOMPARE(runtime->relayPortConnect(
                     &bg, mixChan, QStringLiteral("b%1").arg(i),
                     QString(), QString()), 1);
    // The 64th end is a page end — accepted; the 65th of EITHER
    // kind is refused, and pagePortAccept obeys the bound too.
    QVERIFY(runtime->pagePortConnect(
                agent, extId, mixChan, QStringLiteral("p63"),
                QString(), QJsonObject())
                .value(QLatin1String("ok")).toBool());
    QCOMPARE(runtime->pagePortConnect(
                 agent, extId, mixChan, QStringLiteral("p64"),
                 QString(), QJsonObject())
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("too-many-ends"));
    QCOMPARE(runtime->pagePortAccept(
                 agent, mixChan, QStringLiteral("pa"))
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("too-many-ends"));
    QCOMPARE(runtime->relayPortConnect(
                 &bg, mixChan, QStringLiteral("b63"),
                 QString(), QString()), 0);
    // Drain the chan so it leaves no live ends behind.
    for (int i = 0; i < 63; ++i)
        runtime->relayPortDisconnect(
            &bg, mixChan, QStringLiteral("b%1").arg(i));
    runtime->pagePortDisconnect(agent, mixChan,
                                QStringLiteral("p63"));

    // Register/disconnect cycles on the page end.
    for (int cycle = 0; cycle < 8; ++cycle) {
        const QString pid = QStringLiteral("pc%1").arg(cycle);
        QVERIFY(runtime->pagePortConnect(
                    agent, extId, pageChan, pid, QString(),
                    QJsonObject())
                    .value(QLatin1String("ok")).toBool());
        runtime->pagePortMessage(agent, pageChan, pid, QJsonValue(1));
        runtime->pagePortDisconnect(agent, pageChan, pid);
    }

    // Page-originated send resolves back onto the agent's extEvent.
    const QString pmid = extId + QStringLiteral("#ps0");
    QVERIFY(runtime->pageSend(
                agent, extId, pmid, QJsonValue(1), QJsonObject())
                .value(QLatin1String("ok")).toBool());
    QVERIFY(runtime->relayRespond(&peer, pmid,
                                  QJsonValue(QStringLiteral("pong"))));
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&]() {
            for (const QList<QVariant> &args : agentSpy) {
                if (args.value(0).toString()
                        == QLatin1String("__arktos.extResponse")
                    && args.value(1).toJsonObject()
                           .value(QLatin1String("mid")).toString()
                       == pmid)
                    return true;
            }
            return false;
        }()), 10000);

    // MRG10/SEC26-3: page-originated sends obey the same dup-mid
    // rule as relaySend — a live mid is refused outright instead of
    // overwriting the pending entry, and the FIRST send still
    // resolves normally.
    const QString dupMid = extId + QStringLiteral("#dup0");
    QVERIFY(runtime->pageSend(
                agent, extId, dupMid, QJsonValue(1), QJsonObject())
                .value(QLatin1String("ok")).toBool());
    QCOMPARE(runtime->pageSend(
                 agent, extId, dupMid, QJsonValue(2), QJsonObject())
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("duplicate-mid"));
    QVERIFY(runtime->relayRespond(
                &peer, dupMid, QJsonValue(QStringLiteral("first"))));
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&]() {
            for (const QList<QVariant> &args : agentSpy) {
                if (args.value(0).toString()
                        == QLatin1String("__arktos.extResponse")
                    && args.value(1).toJsonObject()
                           .value(QLatin1String("mid")).toString()
                       == dupMid
                    && args.value(1).toJsonObject()
                           .value(QLatin1String("response")).toString()
                       == QLatin1String("first"))
                    return true;
            }
            return false;
        }()), 10000);

    // Host->page delivery + respond round trip: bg waits, the agent
    // answers, the reply resolves as '__arktos.deliverResponse'.
    const QString dmid = extId + QStringLiteral("#pd0");
    runtime->pageDeliver(agent, extId, dmid, QJsonValue(2),
                         QJsonObject(), &bg);
    QVERIFY(runtime->pageRespond(agent, dmid,
                                 QJsonValue(QStringLiteral("ok")), true)
                .value(QLatin1String("ok")).toBool());
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&]() {
            for (const QList<QVariant> &args : bgSpy) {
                if (args.value(0).toString()
                        == QLatin1String("__arktos.deliverResponse")
                    && args.value(1).toJsonObject()
                           .value(QLatin1String("mid")).toString()
                       == dmid)
                    return true;
            }
            return false;
        }()), 10000);

    // MRG10/SEC26-3: the host->page delivery map has the same
    // in-flight-mid rule — a repeated mid refuses instead of
    // overwriting the live delivery.
    const QString dmid2 = extId + QStringLiteral("#pd1");
    QVERIFY(runtime->pageDeliver(agent, extId, dmid2, QJsonValue(4),
                                 QJsonObject(), &bg));
    QVERIFY(!runtime->pageDeliver(agent, extId, dmid2, QJsonValue(9),
                                  QJsonObject(), &bg));
    QVERIFY(runtime->pageRespond(agent, dmid2,
                                 QJsonValue(QStringLiteral("a")), true)
                .value(QLatin1String("ok")).toBool());

    // The real expire path: one pending mid and one page delivery
    // left unanswered until their 30 s timeout lambdas run — the same
    // teardown unregister takes, through the timer instead.
    const QString xmid = extId + QStringLiteral("#xpire");
    QCOMPARE(runtime->relaySend(&bg, xmid, QJsonObject()), 1);
    const QString dexpire = extId + QStringLiteral("#dxpire");
    QVERIFY(runtime->pageDeliver(agent, extId, dexpire, QJsonValue(3),
                                 QJsonObject(), &bg));
    QTest::qWait(32000);
    // Both entries are gone — late answers find no slot, and the
    // delivery timeout resolved its waiter with the honest
    // no-response shape.
    QVERIFY(!runtime->relayRespond(&peer, xmid, QJsonValue(1)));
    QCOMPARE(runtime->pageRespond(agent, dexpire, QJsonValue(1), false)
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("unknown-mid"));
    {
        bool resolved = false;
        for (const QList<QVariant> &args : bgSpy) {
            if (args.value(0).toString()
                    != QLatin1String("__arktos.deliverResponse"))
                continue;
            const QJsonObject payload = args.value(1).toJsonObject();
            if (payload.value(QLatin1String("mid")).toString() == dexpire
                    && payload.value(QLatin1String("response")).isNull()
                    && !payload.value(QLatin1String("hadListeners"))
                            .toBool())
                resolved = true;
        }
        QVERIFY(resolved);
    }

    // Page-death expire: ~ExtensionPageBridge runs
    // unregisterPageContext — the page's ends and pending sends die
    // with it and survivors get the disconnect push.
    const QString deadChan = extId + QStringLiteral("#dead");
    QVERIFY(runtime->pagePortConnect(
                agent, extId, deadChan, QStringLiteral("d0"),
                QString(), QJsonObject())
                .value(QLatin1String("ok")).toBool());
    QCOMPARE(runtime->relayPortConnect(
                 &bg, deadChan, QStringLiteral("bgd"),
                 QString(), QString()), 1);
    QVERIFY(runtime->pageSend(
                agent, extId, extId + QStringLiteral("#pend"),
                QJsonValue(1), QJsonObject())
                .value(QLatin1String("ok")).toBool());
    // MRG10/SEC26-3: the pending bound is shared — mids minted by the
    // page count against kMaxRelayPending, and once the map is full
    // BOTH send paths refuse ("#pend" above is the one live entry).
    int sent = 0;
    for (int i = 0; i < 600; ++i) {
        if (runtime->pageSend(
                agent, extId,
                extId + QStringLiteral("#fill%1").arg(i),
                QJsonValue(i), QJsonObject())
                .value(QLatin1String("ok")).toBool())
            ++sent;
        else
            break;
    }
    QCOMPARE(sent, 511);
    QCOMPARE(runtime->pageSend(
                 agent, extId, extId + QStringLiteral("#overflow"),
                 QJsonValue(1), QJsonObject())
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("too-many-pending"));
    QCOMPARE(runtime->relaySend(
                 &bg, extId + QStringLiteral("#bfull"),
                 QJsonObject()), 0);
    const QPointer<ExtensionPageBridge> agentGuard(agent);
    const QPointer<QWebEnginePage> pageObjGuard(page);
    view->deleteLater();
    QTRY_VERIFY_WITH_TIMEOUT(!agentGuard && !pageObjGuard, 15000);
    // The agent's ends died with it — survivors got the disconnect
    // push, and only bg's end remains on the chan.
    bool pushedDisconnect = false;
    for (const QList<QVariant> &args : bgSpy) {
        if (args.value(0).toString()
            == QLatin1String("__arktos.portDisconnect"))
            pushedDisconnect = true;
    }
    QVERIFY(pushedDisconnect);
    QCOMPARE(runtime->relayPortMessage(
                 &bg, deadChan, QStringLiteral("bgd"),
                 QJsonValue(1)), 0);
    // The death-path sweep drained every page-origin pending mid
    // (the 512-entry fill above) — the shared map is open to bridge
    // sends again.
    QCOMPARE(runtime->relaySend(
                 &bg, extId + QStringLiteral("#drain"),
                 QJsonObject()), 1);

    runtime->unregisterRelayContext(&bg);
    runtime->unregisterRelayContext(&peer);
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
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
            "\"permissions\":[\"storage\",\"alarms\",\"notifications\","
            "\"contextMenus\"],"
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

    // --- notifications (ER12) — the real record registry over the
    // stub presenter (ARKTOS_EXTNOTIFY_STUB keeps banners off the
    // test display and answers synthetic platform ids) ------------
    qputenv("ARKTOS_EXTNOTIFY_STUB", "1");
    QSignalSpy pushSpy(&backend, &ExtensionShimBackend::pushEvent);
    reply = backend.invoke(
        QStringLiteral("notifications"), QStringLiteral("create"),
        QJsonObject{{QStringLiteral("options"),
                     QJsonObject{{QStringLiteral("type"),
                                  QStringLiteral("basic")},
                                 {QStringLiteral("title"),
                                  QStringLiteral("T")},
                                 {QStringLiteral("message"),
                                  QStringLiteral("M")}}}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    const QString notifId = reply.value(QLatin1String("result"))
        .toObject().value(QLatin1String("id")).toString();
    QVERIFY(!notifId.isEmpty());
    // A bare backend hears event pushes through its own sink — the
    // stub presenter's onShown lands immediately.
    QTRY_COMPARE(pushCount(pushSpy, QStringLiteral(
        "notifications.onShown")), 1);
    QCOMPARE(ExtensionNotificationCenter::instance()
                 ->platformIdFor(id, notifId) != 0, true);
    QVERIFY(ExtensionNotificationCenter::instance()
                ->presented(id, notifId));
    reply = backend.invoke(QStringLiteral("notifications"),
                           QStringLiteral("getAll"), QJsonObject());
    QVERIFY(reply.value(QLatin1String("result")).toObject()
            .contains(notifId));
    reply = backend.invoke(
        QStringLiteral("notifications"), QStringLiteral("update"),
        QJsonObject{{QStringLiteral("id"), notifId},
                    {QStringLiteral("options"),
                     QJsonObject{{QStringLiteral("type"),
                                  QStringLiteral("basic")}}}});
    QCOMPARE(reply.value(QLatin1String("result")).toBool(), true);
    reply = backend.invoke(
        QStringLiteral("notifications"), QStringLiteral("update"),
        QJsonObject{{QStringLiteral("id"),
                     QStringLiteral("missing")},
                    {QStringLiteral("options"),
                     QJsonObject{{QStringLiteral("type"),
                                  QStringLiteral("basic")}}}});
    QCOMPARE(reply.value(QLatin1String("result")).toBool(), false);
    // Chrome's own contract: bad type / missing title / >2 buttons
    // are typed invalid-argument.
    reply = backend.invoke(
        QStringLiteral("notifications"), QStringLiteral("create"),
        QJsonObject{{QStringLiteral("options"),
                     QJsonObject{{QStringLiteral("type"),
                                  QStringLiteral("bogus")},
                                 {QStringLiteral("title"),
                                  QStringLiteral("x")}}}});
    QCOMPARE(reply.value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    // Daemon-style signals route through the public slots — a click
    // is onClicked, 'btn-N' is onButtonClicked.
    const quint32 ntfPid = ExtensionNotificationCenter::instance()
        ->platformIdFor(id, notifId);
    ExtensionNotificationCenter::instance()->onActionInvoked(
        ntfPid, QStringLiteral("btn-0"));
    QTRY_COMPARE(pushCount(pushSpy, QStringLiteral(
        "notifications.onButtonClicked")), 1);
    reply = backend.invoke(
        QStringLiteral("notifications"), QStringLiteral("clear"),
        QJsonObject{{QStringLiteral("id"), notifId}});
    QCOMPARE(reply.value(QLatin1String("result")).toBool(), true);
    QTRY_COMPARE(pushCount(pushSpy, QStringLiteral(
        "notifications.onClosed")), 1);
    QVERIFY(backend.invoke(
        QStringLiteral("notifications"), QStringLiteral("getAll"),
        QJsonObject()).value(QLatin1String("result")).toObject()
            .isEmpty());
    // An extension without the 'notifications' permission gets the
    // typed denial, like every other gated namespace.
    {
        const QString dir2 = dir + QLatin1String("-noperm");
        QVERIFY(QDir().mkpath(dir2));
        QFile mf2(dir2 + QLatin1String("/manifest.json"));
        QVERIFY(mf2.open(QIODevice::WriteOnly));
        mf2.write("{\"manifest_version\":3,\"name\":\"tst-np\","
                  "\"version\":\"1.0\",\"permissions\":[\"storage\"]}");
        mf2.close();
        ExtensionShimBackend bare(
            QStringLiteral("tstmockextensionid0002"),
            QStringLiteral("tst-np"), dir2);
        reply = bare.invoke(QStringLiteral("notifications"),
                            QStringLiteral("create"), QJsonObject());
        QCOMPARE(reply.value(QLatin1String("error")).toString(),
                 QStringLiteral("no-permission"));

        // ER16 — the bridge object answers ANY code in the context,
        // so the call-level gates carry the namespace absence the
        // JS install list only mirrors.  'storage'-only fixture:
        QCOMPARE(bare.invoke(
                     QStringLiteral("contextMenus"),
                     QStringLiteral("create"),
                     QJsonObject{{QStringLiteral("props"),
                                  QJsonObject{{QStringLiteral("id"),
                                               QStringLiteral("x")}}}})
                     .value(QLatin1String("error")).toString(),
                 QStringLiteral("no-permission"));
        QCOMPARE(bare.invoke(
                     QStringLiteral("alarms"),
                     QStringLiteral("create"),
                     QJsonObject{{QStringLiteral("name"),
                                  QStringLiteral("a")}})
                     .value(QLatin1String("error")).toString(),
                 QStringLiteral("no-permission"));
        QCOMPARE(bare.invoke(
                     QStringLiteral("idle"),
                     QStringLiteral("queryState"),
                     QJsonObject{{QStringLiteral("interval"), 60}})
                     .value(QLatin1String("error")).toString(),
                 QStringLiteral("no-permission"));
        QCOMPARE(bare.invoke(
                     QStringLiteral("sidePanel"),
                     QStringLiteral("setOptions"),
                     QJsonObject{{QStringLiteral("options"),
                                  QJsonObject()}})
                     .value(QLatin1String("error")).toString(),
                 QStringLiteral("no-permission"));
        QCOMPARE(bare.invoke(
                     QStringLiteral("sidebarAction"),
                     QStringLiteral("setTitle"),
                     QJsonObject{{QStringLiteral("title"),
                                  QStringLiteral("x")}})
                     .value(QLatin1String("error")).toString(),
                 QStringLiteral("no-permission"));
        // management: every member but getSelf needs the grant.
        QCOMPARE(bare.invoke(
                     QStringLiteral("management"),
                     QStringLiteral("setEnabled"),
                     QJsonObject{{QStringLiteral("id"),
                                  QStringLiteral("x")}})
                     .value(QLatin1String("error")).toString(),
                 QStringLiteral("no-permission"));
        QVERIFY(bare.invoke(QStringLiteral("management"),
                            QStringLiteral("getSelf"), QJsonObject())
                    .value(QLatin1String("ok")).toBool());
        // What IS granted still answers.
        QVERIFY(bare.invoke(
                    QStringLiteral("storage"),
                    QStringLiteral("local.set"),
                    QJsonObject{{QStringLiteral("items"),
                                 QJsonObject{{QStringLiteral("k"), 1}}}})
                    .value(QLatin1String("ok")).toBool());

        // ER16 — navigation scoping: url classes Chrome never
        // accepts from an extension die before a tab/window exists.
        QCOMPARE(bare.invoke(
                     QStringLiteral("tabs"),
                     QStringLiteral("create"),
                     QJsonObject{{QStringLiteral("props"),
                                  QJsonObject{{QStringLiteral("url"),
                                  QStringLiteral(
                                      "javascript:alert(1)")}}}})
                     .value(QLatin1String("error")).toString(),
                 QStringLiteral("url-refused"));
        QCOMPARE(bare.invoke(
                     QStringLiteral("tabs"),
                     QStringLiteral("create"),
                     QJsonObject{{QStringLiteral("props"),
                                  QJsonObject{{QStringLiteral("url"),
                                  QStringLiteral(
                                      "file:///etc/passwd")}}}})
                     .value(QLatin1String("error")).toString(),
                 QStringLiteral("url-refused"));
        QCOMPARE(bare.invoke(
                     QStringLiteral("tabs"),
                     QStringLiteral("create"),
                     QJsonObject{{QStringLiteral("props"),
                                  QJsonObject{{QStringLiteral("url"),
                                  QStringLiteral(
                                      "chrome-extension://"
                                      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                                      "aa/x.html")}}}})
                     .value(QLatin1String("error")).toString(),
                 QStringLiteral("url-refused"));
        QCOMPARE(bare.invoke(
                     QStringLiteral("tabs"),
                     QStringLiteral("create"),
                     QJsonObject{{QStringLiteral("props"),
                                  QJsonObject{{QStringLiteral("url"),
                                  QStringLiteral(
                                      "../escape.html")}}}})
                     .value(QLatin1String("error")).toString(),
                 QStringLiteral("url-refused"));
        QCOMPARE(bare.invoke(
                     QStringLiteral("windows"),
                     QStringLiteral("create"),
                     QJsonObject{{QStringLiteral("props"),
                                  QJsonObject{{QStringLiteral("url"),
                                  QStringLiteral(
                                      "javascript:alert(1)")}}}})
                     .value(QLatin1String("error")).toString(),
                 QStringLiteral("url-refused"));
        QDir(dir2).removeRecursively();
    }

    // ER16 — manifest over-claiming: optional_permissions are inert
    // (request() is typed unsupported), so declaring them must not
    // unlock a gated call nor inflate contains()/getAll().
    {
        const QString dir3 = dir + QLatin1String("-overclaim");
        QVERIFY(QDir().mkpath(dir3));
        QFile mf3(dir3 + QLatin1String("/manifest.json"));
        QVERIFY(mf3.open(QIODevice::WriteOnly));
        mf3.write("{\"manifest_version\":3,\"name\":\"tst-oc\","
                  "\"version\":\"1.0\",\"optional_permissions\":"
                  "[\"storage\",\"tabs\",\"alarms\"]}");
        mf3.close();
        ExtensionShimBackend oc(
            QStringLiteral("tstmockextensionid0003"),
            QStringLiteral("tst-oc"), dir3);
        QCOMPARE(oc.invoke(
                     QStringLiteral("storage"),
                     QStringLiteral("local.set"),
                     QJsonObject{{QStringLiteral("items"),
                                  QJsonObject{{QStringLiteral("k"),
                                               1}}}})
                     .value(QLatin1String("error")).toString(),
                 QStringLiteral("no-permission"));
        QCOMPARE(oc.invoke(
                     QStringLiteral("alarms"),
                     QStringLiteral("create"),
                     QJsonObject{{QStringLiteral("name"),
                                  QStringLiteral("a")}})
                     .value(QLatin1String("error")).toString(),
                 QStringLiteral("no-permission"));
        QCOMPARE(oc.invoke(
                     QStringLiteral("permissions"),
                     QStringLiteral("contains"),
                     QJsonObject{{QStringLiteral("permissions"),
                                  QJsonArray{QStringLiteral(
                                      "storage")}}})
                     .value(QLatin1String("result")).toBool(), false);
        QVERIFY(oc.invoke(
                    QStringLiteral("permissions"),
                    QStringLiteral("getAll"), QJsonObject())
                    .value(QLatin1String("result")).toObject()
                    .value(QLatin1String("permissions")).toArray()
                    .isEmpty());
        QCOMPARE(oc.invoke(
                     QStringLiteral("permissions"),
                     QStringLiteral("request"),
                     QJsonObject{{QStringLiteral("permissions"),
                                  QJsonArray{QStringLiteral(
                                      "storage")}}})
                     .value(QLatin1String("error")).toString(),
                 QStringLiteral("unsupported"));
        QDir(dir3).removeRecursively();
    }

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

// MD19 — chrome.storage extras.  getKeys/setAccessLevel ride the
// same dispatch as get/set; 'sync' persists under extstorage/ like
// 'local' (no cloud sync — a recorded note says so); and Chrome's
// quota bounds (sync QUOTA_BYTES_PER_ITEM 8192 / QUOTA_BYTES 102400
// / MAX_ITEMS 512, local+session 10 MB) refuse oversized writes
// atomically with a typed quota-exceeded.
void tst_ExtensionManager::extensionStorageExtras()
{
    const QString dir = QDir::temp().filePath(
        QStringLiteral("tst-extstore-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dir));
    {
        QFile manifestFile(dir + QLatin1String("/manifest.json"));
        QVERIFY(manifestFile.open(QIODevice::WriteOnly));
        manifestFile.write(
            "{\"manifest_version\":3,\"name\":\"tst-store\","
            "\"version\":\"1.0\",\"permissions\":[\"storage\"]}");
        manifestFile.close();
    }
    const QString id = QStringLiteral("tstmockextensionid0099");

    ExtensionShimBackend backend(id, QStringLiteral("tst-store"), dir);

    // --- sync round trip, no longer the session map ---------------
    QJsonObject reply = backend.invoke(
        QStringLiteral("storage"), QStringLiteral("sync.set"),
        QJsonObject{{QStringLiteral("items"),
                     QJsonObject{{QStringLiteral("k"), 1},
                                 {QStringLiteral("name"),
                                  QStringLiteral("v")}}}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QVERIFY(reply.value(QLatin1String("result")).toObject()
            .value(QLatin1String("changes")).toObject()
            .value(QLatin1String("k")).toObject()
            .contains(QLatin1String("newValue")));
    reply = backend.invoke(
        QStringLiteral("storage"), QStringLiteral("sync.get"),
        QJsonObject{{QStringLiteral("keys"), QStringLiteral("k")}});
    QCOMPARE(reply.value(QLatin1String("result")).toObject()
             .value(QLatin1String("k")).toInt(), 1);
    QVERIFY(backend.invoke(
        QStringLiteral("storage"), QStringLiteral("session.set"),
        QJsonObject{{QStringLiteral("items"),
                     QJsonObject{{QStringLiteral("k"), 9}}}})
        .value(QLatin1String("ok")).toBool());
    reply = backend.invoke(
        QStringLiteral("storage"), QStringLiteral("sync.get"),
        QJsonObject{{QStringLiteral("keys"), QStringLiteral("k")}});
    QCOMPARE(reply.value(QLatin1String("result")).toObject()
             .value(QLatin1String("k")).toInt(), 1);
    reply = backend.invoke(
        QStringLiteral("storage"), QStringLiteral("session.get"),
        QJsonObject{{QStringLiteral("keys"),
                     QStringLiteral("name")}});
    QVERIFY(!reply.value(QLatin1String("result")).toObject()
            .contains(QLatin1String("name")));

    // --- getKeys ---------------------------------------------------
    reply = backend.invoke(
        QStringLiteral("storage"), QStringLiteral("sync.getKeys"),
        QJsonObject());
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QStringList keys;
    for (const QJsonValue &v
         : reply.value(QLatin1String("result")).toArray())
        keys.append(v.toString());
    keys.sort();
    QCOMPARE(keys, (QStringList{QLatin1String("k"),
                                QLatin1String("name")}));
    reply = backend.invoke(
        QStringLiteral("storage"), QStringLiteral("managed.getKeys"),
        QJsonObject());
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QCOMPARE(reply.value(QLatin1String("result")).toArray().size(), 0);
    // The dedicated managed store never aliases a sibling area.
    reply = backend.invoke(
        QStringLiteral("storage"), QStringLiteral("managed.get"),
        QJsonObject{{QStringLiteral("keys"), QJsonValue::Null}});
    QVERIFY(reply.value(QLatin1String("result")).toObject().isEmpty());

    // --- setAccessLevel recorded ------------------------------------
    reply = backend.invoke(
        QStringLiteral("storage"),
        QStringLiteral("session.setAccessLevel"),
        QJsonObject{{QStringLiteral("accessLevel"),
                     QStringLiteral("TRUSTED_AND_UNTRUSTED_CONTEXTS")}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    // Chrome's {accessLevel: level} object spelling lands the same.
    reply = backend.invoke(
        QStringLiteral("storage"),
        QStringLiteral("session.setAccessLevel"),
        QJsonObject{{QStringLiteral("accessLevel"),
                     QJsonObject{{QStringLiteral("accessLevel"),
                                  QStringLiteral("TRUSTED_CONTEXTS")}}}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    reply = backend.invoke(
        QStringLiteral("storage"),
        QStringLiteral("local.setAccessLevel"),
        QJsonObject{{QStringLiteral("accessLevel"),
                     QStringLiteral("BOGUS")}});
    QCOMPARE(reply.value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    bool sawAccessLevel = false, sawNoSync = false;
    for (const QString &n : backend.notes()) {
        if (n.contains(QLatin1String("TRUSTED_AND_UNTRUSTED_CONTEXTS")))
            sawAccessLevel = true;
        if (n.contains(QLatin1String("no cloud sync")))
            sawNoSync = true;
    }
    QVERIFY(sawAccessLevel);
    QVERIFY(sawNoSync);

    // --- quota bounds ------------------------------------------------
    // sync per-item cap: key length + JSON value over 8192 bytes.
    reply = backend.invoke(
        QStringLiteral("storage"), QStringLiteral("sync.set"),
        QJsonObject{{QStringLiteral("items"),
                     QJsonObject{{QStringLiteral("big"),
                                  QString(9000, QLatin1Char('x'))}}}});
    QCOMPARE(reply.value(QLatin1String("error")).toString(),
             QStringLiteral("quota-exceeded"));
    reply = backend.invoke(
        QStringLiteral("storage"), QStringLiteral("sync.get"),
        QJsonObject{{QStringLiteral("keys"), QStringLiteral("big")}});
    QVERIFY(!reply.value(QLatin1String("result")).toObject()
            .contains(QLatin1String("big")));
    // sync total cap: under-item-size items whose sum crosses 100KB.
    QJsonObject many;
    for (int i = 0; i < 20; ++i)
        many.insert(QStringLiteral("f%1").arg(i),
                    QString(7000, QLatin1Char('y')));
    reply = backend.invoke(
        QStringLiteral("storage"), QStringLiteral("sync.set"),
        QJsonObject{{QStringLiteral("items"), many}});
    QCOMPARE(reply.value(QLatin1String("error")).toString(),
             QStringLiteral("quota-exceeded"));
    // MAX_ITEMS: one call carrying more than 512 distinct keys.
    QJsonObject flood;
    for (int i = 0; i < 513; ++i)
        flood.insert(QStringLiteral("i%1").arg(i), i);
    reply = backend.invoke(
        QStringLiteral("storage"), QStringLiteral("sync.set"),
        QJsonObject{{QStringLiteral("items"), flood}});
    QCOMPARE(reply.value(QLatin1String("error")).toString(),
             QStringLiteral("quota-exceeded"));
    // local's 10 MB ceiling is enforced the same way.
    reply = backend.invoke(
        QStringLiteral("storage"), QStringLiteral("local.set"),
        QJsonObject{{QStringLiteral("items"),
                     QJsonObject{{QStringLiteral("huge"),
                                  QString(11 * 1024 * 1024,
                                          QLatin1Char('z'))}}}});
    QCOMPARE(reply.value(QLatin1String("error")).toString(),
             QStringLiteral("quota-exceeded"));

    // --- sync survives a backend respawn ----------------------------
    QVERIFY(QFile::exists(
        BrowserPaths::dataFilePath(QStringLiteral("extstorage"))
        + QLatin1Char('/') + id + QLatin1String("/sync.json")));
    {
        ExtensionShimBackend again(id, QStringLiteral("tst-store"),
                                   dir);
        reply = again.invoke(
            QStringLiteral("storage"), QStringLiteral("sync.get"),
            QJsonObject{{QStringLiteral("keys"),
                         QStringLiteral("name")}});
        QCOMPARE(reply.value(QLatin1String("result")).toObject()
                 .value(QLatin1String("name")).toString(),
                 QStringLiteral("v"));
        reply = again.invoke(
            QStringLiteral("storage"), QStringLiteral("sync.getKeys"),
            QJsonObject());
        QCOMPARE(reply.value(QLatin1String("result")).toArray()
                 .size(), 2);
        // session stays volatile — the respawn sees none of it.
        reply = again.invoke(
            QStringLiteral("storage"), QStringLiteral("session.get"),
            QJsonObject{{QStringLiteral("keys"),
                         QStringLiteral("k")}});
        QVERIFY(!reply.value(QLatin1String("result")).toObject()
                .contains(QLatin1String("k")));
    }

    QDir(BrowserPaths::dataFilePath(
        QStringLiteral("extstorage")) + QLatin1Char('/') + id)
        .removeRecursively();
    QDir(dir).removeRecursively();
}

// MD20 — chrome.alarms edge parity on a bare backend: Chrome's
// create() argument checks (when+delayInMinutes together or no
// schedule at all refuse; the name may ride alarmInfo itself but
// never both forms at once), unnamed alarms live under Chrome's ""
// default name (get/clear/onAlarm all serve it, a second unnamed
// create replaces), the MV3 30s periodInMinutes floor clamps with
// a recorded note, and getAll answers in creation order with a
// replaced alarm re-appending at the end.
void tst_ExtensionManager::extensionAlarmEdges()
{
    const QString dir = QDir::temp().filePath(
        QStringLiteral("tst-extalarm-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dir));
    {
        QFile f(dir + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-alarm\","
                "\"version\":\"1.0\",\"permissions\":[\"alarms\"]}");
        f.close();
    }
    ExtensionShimBackend backend(
        QStringLiteral("tstmockextensionid0004"),
        QStringLiteral("tst-alarm"), dir);

    auto create = [&backend](const QString &name,
                             const QJsonObject &info) {
        return backend.invoke(
            QStringLiteral("alarms"), QStringLiteral("create"),
            QJsonObject{{QStringLiteral("name"), name},
                        {QStringLiteral("info"), info}});
    };
    auto getAllNames = [&backend]() {
        QStringList names;
        const QJsonArray all = backend.invoke(
            QStringLiteral("alarms"), QStringLiteral("getAll"),
            QJsonObject()).value(QLatin1String("result")).toArray();
        for (const QJsonValue &v : all)
            names.append(v.toObject().value(
                QStringLiteral("name")).toString());
        return names;
    };

    // --- Chrome's create() argument validation --------------------
    QCOMPARE(create(QStringLiteral("both"),
                    QJsonObject{{QStringLiteral("when"), 1.0},
                                {QStringLiteral("delayInMinutes"),
                                 1.0}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    QCOMPARE(create(QStringLiteral("none"), QJsonObject())
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    QCOMPARE(create(QStringLiteral("arg"),
                    QJsonObject{{QStringLiteral("name"),
                                 QStringLiteral("obj")},
                                {QStringLiteral("delayInMinutes"),
                                 60.0}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    QVERIFY(getAllNames().isEmpty());

    // --- unnamed alarms under Chrome's "" default -----------------
    QVERIFY(create(QString(), QJsonObject{
            {QStringLiteral("delayInMinutes"), 30.0}})
        .value(QLatin1String("ok")).toBool());
    QJsonObject reply = backend.invoke(
        QStringLiteral("alarms"), QStringLiteral("get"),
        QJsonObject{{QStringLiteral("name"), QString()}});
    QVERIFY(reply.value(QLatin1String("result")).isObject());
    QCOMPARE(reply.value(QLatin1String("result")).toObject()
             .value(QLatin1String("name")).toString(), QString());
    // A second unnamed create replaces the first like Chrome —
    // getAll still shows exactly one "" alarm.
    QVERIFY(create(QString(), QJsonObject{
            {QStringLiteral("delayInMinutes"), 45.0}})
        .value(QLatin1String("ok")).toBool());
    QCOMPARE(getAllNames(), (QStringList{QString()}));
    // The unnamed alarm fires onAlarm with its "" name.
    QSignalSpy fireSpy(&backend, &ExtensionShimBackend::pushEvent);
    QVERIFY(create(QString(), QJsonObject{
            {QStringLiteral("delayInMinutes"), 0.002}})
        .value(QLatin1String("ok")).toBool());
    QTRY_VERIFY_WITH_TIMEOUT(fireSpy.size() >= 1, 10000);
    QCOMPARE(fireSpy.first().at(0).toString(),
             QStringLiteral("alarms.onAlarm"));
    QCOMPARE(fireSpy.first().at(1).toJsonObject()
             .value(QLatin1String("name")).toString(), QString());
    // The fired one-shot already self-removed like Chrome; clear('')
    // on a live unnamed alarm answers true.
    QVERIFY(create(QString(), QJsonObject{
            {QStringLiteral("delayInMinutes"), 60.0}})
        .value(QLatin1String("ok")).toBool());
    reply = backend.invoke(
        QStringLiteral("alarms"), QStringLiteral("clear"),
        QJsonObject{{QStringLiteral("name"), QString()}});
    QCOMPARE(reply.value(QLatin1String("result")).toBool(), true);

    // --- object-form alarmInfo.name rides the create --------------
    QVERIFY(create(QString(), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("objname")},
            {QStringLiteral("delayInMinutes"), 30.0}})
        .value(QLatin1String("ok")).toBool());
    reply = backend.invoke(
        QStringLiteral("alarms"), QStringLiteral("get"),
        QJsonObject{{QStringLiteral("name"),
                     QStringLiteral("objname")}});
    QCOMPARE(reply.value(QLatin1String("result")).toObject()
             .value(QLatin1String("name")).toString(),
             QStringLiteral("objname"));

    // --- MV3 30s period floor + recorded note ----------------------
    QVERIFY(create(QStringLiteral("spin"),
                   QJsonObject{{QStringLiteral("delayInMinutes"),
                                30.0},
                               {QStringLiteral("periodInMinutes"),
                                0.1}})
        .value(QLatin1String("ok")).toBool());
    reply = backend.invoke(
        QStringLiteral("alarms"), QStringLiteral("get"),
        QJsonObject{{QStringLiteral("name"),
                     QStringLiteral("spin")}});
    QCOMPARE(reply.value(QLatin1String("result")).toObject()
             .value(QLatin1String("periodInMinutes")).toDouble(),
             0.5);
    const int noteCount = backend.notes().size();
    bool sawFloorNote = false;
    for (const QString &n : backend.notes())
        if (n.contains(QLatin1String("30-second"))
                && n.contains(QLatin1String("spin")))
            sawFloorNote = true;
    QVERIFY(sawFloorNote);
    // A period at the floor records no note.
    QVERIFY(create(QStringLiteral("calm"),
                   QJsonObject{{QStringLiteral("delayInMinutes"),
                                30.0},
                               {QStringLiteral("periodInMinutes"),
                                1.0}})
        .value(QLatin1String("ok")).toBool());
    QCOMPARE(backend.notes().size(), noteCount);

    // --- periodInMinutes doubles as delay when no first-fire ------
    QVERIFY(create(QStringLiteral("peronly"),
                   QJsonObject{{QStringLiteral("periodInMinutes"),
                                2.0}})
        .value(QLatin1String("ok")).toBool());
    reply = backend.invoke(
        QStringLiteral("alarms"), QStringLiteral("get"),
        QJsonObject{{QStringLiteral("name"),
                     QStringLiteral("peronly")}});
    QVERIFY(reply.value(QLatin1String("result")).toObject()
            .value(QLatin1String("scheduledTime")).toDouble()
            > QDateTime::currentMSecsSinceEpoch() + 100000.0);

    // --- getAll answers in Chrome's creation order ----------------
    backend.invoke(QStringLiteral("alarms"),
                   QStringLiteral("clearAll"), QJsonObject());
    QVERIFY(create(QStringLiteral("first"),
                   QJsonObject{{QStringLiteral("delayInMinutes"),
                                30.0}}).value(QLatin1String("ok"))
            .toBool());
    QVERIFY(create(QStringLiteral("second"),
                   QJsonObject{{QStringLiteral("delayInMinutes"),
                                40.0}}).value(QLatin1String("ok"))
            .toBool());
    QVERIFY(create(QStringLiteral("third"),
                   QJsonObject{{QStringLiteral("delayInMinutes"),
                                50.0}}).value(QLatin1String("ok"))
            .toBool());
    QCOMPARE(getAllNames(), (QStringList{
        QStringLiteral("first"), QStringLiteral("second"),
        QStringLiteral("third")}));
    // A replaced alarm re-appends at the end like Chrome's list.
    QVERIFY(create(QStringLiteral("first"),
                   QJsonObject{{QStringLiteral("delayInMinutes"),
                                35.0}}).value(QLatin1String("ok"))
            .toBool());
    QCOMPARE(getAllNames(), (QStringList{
        QStringLiteral("second"), QStringLiteral("third"),
        QStringLiteral("first")}));

    QDir(dir).removeRecursively();
}

// MD10 — chrome.tabs core ops + state honesty.  A bare
// ExtensionShimBackend drives every op against a real
// BrowserMainWindow's strip: remove/reload/duplicate/highlight/
// move/goBack/goForward/dispatch + the update() state setters, and
// the tabObject fields Chrome reads (pinned/discarded/groupId/
// mutedInfo/audible/autoDiscardable) answer real strip and page
// state instead of constants.  Query filters select on the same
// fields.  Everything is asserted through the bridge reply shape
// ({ok,result} / {ok:false,error}) the shim consumes.
void tst_ExtensionManager::extensionTabsOps()
{
    const QString dir = QDir::temp().filePath(
        QStringLiteral("tst-exttabs-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dir));
    {
        QFile f(dir + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-tabs\","
                "\"version\":\"1.0\",\"permissions\":[\"tabs\"]}");
        f.close();
    }
    ExtensionShimBackend backend(
        QStringLiteral("tstmockextensionid0003"),
        QStringLiteral("tst-tabs"), dir);

    BrowserMainWindow *window = new BrowserMainWindow;
    TabWidget *tw = window->tabWidget();
    QVERIFY(tw && tw->count() >= 1);
    WebView *view0 = tw->webView(0);
    QVERIFY(view0 && view0->page());
    WebView *view1 = tw->makeNewTab(false);
    WebView *view2 = tw->makeNewTab(false);
    QVERIFY(view1 && view2);
    QCOMPARE(tw->count(), 3);
    // A committed url apiece — discard refuses the empty-page case
    // and the ops deserve a real document underneath.  Wait on
    // loadFinished spies: WebView::isLoading() is false before the
    // load even starts, so a "not loading" poll is a no-op here.
    QSignalSpy load0(view0->page(), &QWebEnginePage::loadFinished);
    QSignalSpy load1(view1->page(), &QWebEnginePage::loadFinished);
    QSignalSpy load2(view2->page(), &QWebEnginePage::loadFinished);
    view0->load(QUrl(QStringLiteral("data:text/html,<title>t0</title>")));
    view1->load(QUrl(QStringLiteral("data:text/html,<title>t1</title>")));
    view2->load(QUrl(QStringLiteral("data:text/html,<title>t2</title>")));
    QTRY_VERIFY_WITH_TIMEOUT(!load0.isEmpty() && !load1.isEmpty()
                             && !load2.isEmpty(), 30000);

    const int id0 = ExtensionShimBackend::tabIdFor(view0);
    const int id1 = ExtensionShimBackend::tabIdFor(view1);
    const int id2 = ExtensionShimBackend::tabIdFor(view2);
    const int wid = ExtensionShimBackend::windowIdFor(window);
    QVERIFY(id0 > 0 && id1 > 0 && id2 > 0 && wid > 0);

    // --- state honesty ------------------------------------------------
    // Fields Chrome reads answer real state: pinned/groupId start
    // false/-1, then flip through real strip ops.
    QJsonObject reply = backend.invoke(
        QStringLiteral("tabs"), QStringLiteral("get"),
        QJsonObject{{QStringLiteral("id"), id0}});
    QCOMPARE(reply.value(QLatin1String("result")).toObject()
                 .value(QLatin1String("pinned")).toBool(), false);
    QCOMPARE(reply.value(QLatin1String("result")).toObject()
                 .value(QLatin1String("groupId")).toInt(), -1);
    QCOMPARE(reply.value(QLatin1String("result")).toObject()
                 .value(QLatin1String("discarded")).toBool(), false);

    // update({pinned:true}) rides setTabPinned — the strip shows it
    // and the next get reports it.
    reply = backend.invoke(
        QStringLiteral("tabs"), QStringLiteral("update"),
        QJsonObject{{QStringLiteral("id"), id0},
                    {QStringLiteral("props"),
                     QJsonObject{{QStringLiteral("pinned"), true}}}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QVERIFY(tw->isTabPinned(0));
    QCOMPARE(reply.value(QLatin1String("result")).toObject()
                 .value(QLatin1String("pinned")).toBool(), true);

    // A real group: the minted int answers a stable, non-(-1) id and
    // query({groupId}) selects exactly the member.
    const QString gid = tw->createTabGroup(2);
    QVERIFY(!gid.isEmpty());
    reply = backend.invoke(
        QStringLiteral("tabs"), QStringLiteral("get"),
        QJsonObject{{QStringLiteral("id"), id2}});
    const int chromeGid = reply.value(QLatin1String("result"))
        .toObject().value(QLatin1String("groupId")).toInt(-1);
    QVERIFY(chromeGid >= 0);
    reply = backend.invoke(
        QStringLiteral("tabs"), QStringLiteral("query"),
        QJsonObject{{QStringLiteral("query"),
                     QJsonObject{{QStringLiteral("windowId"), wid},
                                 {QStringLiteral("groupId"),
                                  chromeGid}}}});
    QCOMPARE(reply.value(QLatin1String("result")).toArray().size(), 1);

    // update({muted:true}) reaches the engine page — mutedInfo
    // reports it and query({muted}) selects on it.
    reply = backend.invoke(
        QStringLiteral("tabs"), QStringLiteral("update"),
        QJsonObject{{QStringLiteral("id"), id1},
                    {QStringLiteral("props"),
                     QJsonObject{{QStringLiteral("muted"), true}}}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QVERIFY(view1->page()->isAudioMuted());
    reply = backend.invoke(
        QStringLiteral("tabs"), QStringLiteral("query"),
        QJsonObject{{QStringLiteral("query"),
                     QJsonObject{{QStringLiteral("windowId"), wid},
                                 {QStringLiteral("muted"), true}}}});
    QCOMPARE(reply.value(QLatin1String("result")).toArray().size(), 1);

    // --- discard + autoDiscardable ------------------------------------
    // Sleeping IS Chrome's discarded state; discard on a background
    // tab lands the sleep, the active tab answers Chrome's
    // undefined (null result), and autoDiscardable:false wakes it.
    tw->setCurrentIndex(0);
    reply = backend.invoke(
        QStringLiteral("tabs"), QStringLiteral("discard"),
        QJsonObject{{QStringLiteral("id"), id1}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QTRY_VERIFY_WITH_TIMEOUT(tw->isTabSleeping(1), 10000);
    reply = backend.invoke(
        QStringLiteral("tabs"), QStringLiteral("get"),
        QJsonObject{{QStringLiteral("id"), id1}});
    QCOMPARE(reply.value(QLatin1String("result")).toObject()
                 .value(QLatin1String("discarded")).toBool(), true);
    reply = backend.invoke(
        QStringLiteral("tabs"), QStringLiteral("query"),
        QJsonObject{{QStringLiteral("query"),
                     QJsonObject{{QStringLiteral("windowId"), wid},
                                 {QStringLiteral("discarded"), true}}}});
    QCOMPARE(reply.value(QLatin1String("result")).toArray().size(), 1);
    // The active tab cannot be discarded — Chrome's answer is no tab.
    reply = backend.invoke(
        QStringLiteral("tabs"), QStringLiteral("discard"),
        QJsonObject{{QStringLiteral("id"), id0}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QVERIFY(reply.value(QLatin1String("result")).isNull());
    // autoDiscardable:false records the flag AND un-discards.
    reply = backend.invoke(
        QStringLiteral("tabs"), QStringLiteral("update"),
        QJsonObject{{QStringLiteral("id"), id1},
                    {QStringLiteral("props"),
                     QJsonObject{{QStringLiteral("autoDiscardable"),
                                  false}}}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QTRY_VERIFY_WITH_TIMEOUT(!tw->isTabSleeping(1), 10000);
    QCOMPARE(reply.value(QLatin1String("result")).toObject()
                 .value(QLatin1String("autoDiscardable")).toBool(),
             false);

    // --- duplicate / move / highlight ---------------------------------
    // The duplicate lands right after its source, the move reorders
    // the strip, highlight activates and reports the window.
    reply = backend.invoke(
        QStringLiteral("tabs"), QStringLiteral("duplicate"),
        QJsonObject{{QStringLiteral("id"), id2}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    const QJsonObject dup = reply.value(QLatin1String("result"))
        .toObject();
    QVERIFY(dup.value(QLatin1String("id")).toInt() != id2);
    QCOMPARE(tw->count(), 4);
    // Chrome lands the duplicate right after its source.
    QCOMPARE(dup.value(QLatin1String("index")).toInt(),
             tw->webViewIndex(view2) + 1);
    reply = backend.invoke(
        QStringLiteral("tabs"), QStringLiteral("move"),
        QJsonObject{{QStringLiteral("ids"), QJsonArray{id1}},
                    {QStringLiteral("index"), 0},
                    {QStringLiteral("windowId"), wid}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QCOMPARE(reply.value(QLatin1String("result")).toObject()
                 .value(QLatin1String("index")).toInt(), 0);
    QCOMPARE(tw->webViewIndex(view1), 0);
    // Multi-id move answers the array shape.
    reply = backend.invoke(
        QStringLiteral("tabs"), QStringLiteral("move"),
        QJsonObject{{QStringLiteral("ids"), QJsonArray{id1, id2}},
                    {QStringLiteral("index"), 1},
                    {QStringLiteral("windowId"), wid}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QCOMPARE(reply.value(QLatin1String("result")).toArray().size(), 2);
    // Cross-window refusal classes stay typed.
    QCOMPARE(backend.invoke(
                 QStringLiteral("tabs"), QStringLiteral("move"),
                 QJsonObject{{QStringLiteral("ids"), QJsonArray{id1}},
                             {QStringLiteral("index"), 0},
                             {QStringLiteral("windowId"), 424242}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("no-window"));
    reply = backend.invoke(
        QStringLiteral("tabs"), QStringLiteral("highlight"),
        QJsonObject{{QStringLiteral("windowId"), wid},
                    {QStringLiteral("indexes"), QJsonArray{2}}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QCOMPARE(tw->currentIndex(), 2);
    QCOMPARE(reply.value(QLatin1String("result")).toObject()
                 .value(QLatin1String("id")).toInt(), wid);
    QCOMPARE(reply.value(QLatin1String("result")).toObject()
                 .value(QLatin1String("tabs")).toArray().size(), 1);
    QCOMPARE(backend.invoke(
                 QStringLiteral("tabs"), QStringLiteral("highlight"),
                 QJsonObject{{QStringLiteral("windowId"), 424242},
                             {QStringLiteral("indexes"),
                              QJsonArray{0}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("no-window"));

    // --- reload / goBack / goForward ----------------------------------
    reply = backend.invoke(
        QStringLiteral("tabs"), QStringLiteral("reload"),
        QJsonObject{{QStringLiteral("id"), id0}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    // Two committed navigations make a real back entry — the ops
    // then move the page through its history.
    WebView *navTab = tw->webView(1);
    QVERIFY(navTab);
    const int navId = ExtensionShimBackend::tabIdFor(navTab);
    QSignalSpy navSpy(navTab->page(), &QWebEnginePage::loadFinished);
    navTab->load(QUrl(QStringLiteral("data:text/html,<title>n1</title>")));
    QTRY_VERIFY_WITH_TIMEOUT(navSpy.count() >= 1, 30000);
    navTab->load(QUrl(QStringLiteral("data:text/html,<title>n2</title>")));
    QTRY_VERIFY_WITH_TIMEOUT(navSpy.count() >= 2, 30000);
    QTRY_VERIFY_WITH_TIMEOUT(
        navTab->enginePage()->canGoBack(), 10000);
    reply = backend.invoke(
        QStringLiteral("tabs"), QStringLiteral("goBack"),
        QJsonObject{{QStringLiteral("id"), navId}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QTRY_VERIFY_WITH_TIMEOUT(
        navTab->enginePage()->canGoForward(), 10000);
    reply = backend.invoke(
        QStringLiteral("tabs"), QStringLiteral("goForward"),
        QJsonObject{{QStringLiteral("id"), navId}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    // A tab with no forward entry answers the typed refusal.
    reply = backend.invoke(
        QStringLiteral("tabs"), QStringLiteral("goForward"),
        QJsonObject{{QStringLiteral("id"), navId}});
    QCOMPARE(reply.value(QLatin1String("error")).toString(),
             QStringLiteral("no-history"));

    // --- remove --------------------------------------------------------
    const int before = tw->count();
    reply = backend.invoke(
        QStringLiteral("tabs"), QStringLiteral("remove"),
        QJsonObject{{QStringLiteral("ids"), QJsonArray{id0}}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QCOMPARE(tw->count(), before - 1);
    QCOMPARE(backend.invoke(
                 QStringLiteral("tabs"), QStringLiteral("remove"),
                 QJsonObject{{QStringLiteral("ids"),
                              QJsonArray{424242}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("not-found"));

    // windowType: only "normal" matches — every Arktos browser
    // window reports it.
    reply = backend.invoke(
        QStringLiteral("tabs"), QStringLiteral("query"),
        QJsonObject{{QStringLiteral("query"),
                     QJsonObject{{QStringLiteral("windowId"), wid},
                                 {QStringLiteral("windowType"),
                                  QStringLiteral("popup")}}}});
    QCOMPARE(reply.value(QLatin1String("result")).toArray().size(), 0);
    reply = backend.invoke(
        QStringLiteral("tabs"), QStringLiteral("query"),
        QJsonObject{{QStringLiteral("query"),
                     QJsonObject{{QStringLiteral("windowId"), wid},
                                 {QStringLiteral("windowType"),
                                  QStringLiteral("normal")}}}});
    QCOMPARE(reply.value(QLatin1String("result")).toArray().size(),
             tw->count());

    // deleteLater, not close(): multiple open tabs trip the
    // confirmClosingMultipleTabs QMessageBox — modal, unanswerable.
    window->deleteLater();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QDir(dir).removeRecursively();
}

// MD11 — chrome.tabs zoom + captureVisibleTab.  A hosted fixture
// holding 'tabs' sees the tab's pixels (the seesUrls shortcut); a
// backend whose id the runtime never granted 'tabs'/host coverage
// is refused no-permission.  setZoom rides the view's own zoom
// machinery, and setZoomSettings records mode/scope with
// 'disabled' refusing the extension's own setZoom.
void tst_ExtensionManager::extensionTabsZoomCapture()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");

    const QString dir = QDir::temp().filePath(
        QStringLiteral("tst-extzoom-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dir));
    {
        QFile f(dir + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-zoomcap\","
                "\"version\":\"1.0\",\"permissions\":[\"tabs\"],"
                "\"background\":{\"service_worker\":\"sw.js\"}}");
        f.close();
        QFile w(dir + QLatin1String("/sw.js"));
        QVERIFY(w.open(QIODevice::WriteOnly));
        w.write("// zoomcap fixture\n");
        w.close();
    }

    ExtensionManager *manager = ExtensionManager::instance();
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();
    auto loaded = std::make_shared<QString>();
    connect(manager, &ExtensionManager::extensionLoaded, this,
            [loaded](const ExtensionManager::ExtensionInfo &info) {
        if (info.name == QLatin1String("tst-zoomcap")
                && info.error.isEmpty())
            *loaded = info.id;
    });
    manager->grantConsent(dir);
    manager->loadExtension(dir);
    QElapsedTimer timer;
    timer.start();
    while (loaded->isEmpty() && timer.elapsed() < 30000)
        QTest::qWait(50);
    const QString extId = *loaded;
    QVERIFY2(!extId.isEmpty(), "fixture loaded + hosted");
    manager->setExtensionEnabled(extId, true);
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime->host(extId)
            && runtime->host(extId)->state()
                   == ExtensionBackgroundHost::State::Ready,
        45000);
    ExtensionShimBackend *backend =
        runtime->host(extId)->bridge()->backend();
    QVERIFY(backend);

    BrowserMainWindow *window = new BrowserMainWindow;
    TabWidget *tw = window->tabWidget();
    QVERIFY(tw && tw->count() >= 1);
    WebView *view = tw->webView(0);
    QVERIFY(view && view->page());
    QSignalSpy loadSpy(view->page(), &QWebEnginePage::loadFinished);
    view->load(QUrl(QStringLiteral("data:text/html,<title>z</title>")));
    QTRY_VERIFY_WITH_TIMEOUT(!loadSpy.isEmpty(), 30000);
    // The capture needs a sized, realized view — under the
    // offscreen platform show() still drives the compositor.
    window->show();
    QTest::qWaitForWindowExposed(window);
    const int tabId = ExtensionShimBackend::tabIdFor(view);
    const int wid = ExtensionShimBackend::windowIdFor(window);
    QVERIFY(tabId > 0 && wid > 0);

    // --- zoom roundtrip ---------------------------------------------
    QJsonObject reply = backend->invoke(
        QStringLiteral("tabs"), QStringLiteral("getZoom"),
        QJsonObject{{QStringLiteral("id"), tabId}});
    QCOMPARE(reply.value(QLatin1String("result")).toDouble(), 1.0);

    QSignalSpy zoomEvents(backend,
                          &ExtensionShimBackend::pushEvent);
    reply = backend->invoke(
        QStringLiteral("tabs"), QStringLiteral("setZoom"),
        QJsonObject{{QStringLiteral("id"), tabId},
                    {QStringLiteral("zoom"), 1.5}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QCOMPARE(view->currentZoom(), 150);
    reply = backend->invoke(
        QStringLiteral("tabs"), QStringLiteral("getZoom"),
        QJsonObject{{QStringLiteral("id"), tabId}});
    QCOMPARE(reply.value(QLatin1String("result")).toDouble(), 1.5);
    // Chrome's onZoomChange push carries old/new factors + settings.
    QCOMPARE(zoomEvents.size(), 1);
    QCOMPARE(zoomEvents.first().at(0).toString(),
             QStringLiteral("tabs.onZoomChange"));
    const QJsonObject zoomInfo =
        zoomEvents.first().at(1).toJsonObject();
    QCOMPARE(zoomInfo.value(QLatin1String("tabId")).toInt(), tabId);
    QCOMPARE(zoomInfo.value(QLatin1String("newZoomFactor"))
                 .toDouble(), 1.5);
    // Typed refusals: a non-positive factor, a dead tab id.
    QCOMPARE(backend->invoke(
                 QStringLiteral("tabs"), QStringLiteral("setZoom"),
                 QJsonObject{{QStringLiteral("id"), tabId},
                             {QStringLiteral("zoom"), 0.0}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    QCOMPARE(backend->invoke(
                 QStringLiteral("tabs"), QStringLiteral("getZoom"),
                 QJsonObject{{QStringLiteral("id"), 424242}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("not-found"));

    // --- zoomSettings: the recorded model -----------------------------
    reply = backend->invoke(
        QStringLiteral("tabs"), QStringLiteral("getZoomSettings"),
        QJsonObject{{QStringLiteral("id"), tabId}});
    const QJsonObject defaults =
        reply.value(QLatin1String("result")).toObject();
    QCOMPARE(defaults.value(QLatin1String("mode")).toString(),
             QStringLiteral("manual"));
    QCOMPARE(defaults.value(QLatin1String("scope")).toString(),
             QStringLiteral("per-tab"));
    QCOMPARE(defaults.value(QLatin1String("defaultZoomFactor"))
                 .toDouble(), 1.0);
    reply = backend->invoke(
        QStringLiteral("tabs"), QStringLiteral("setZoomSettings"),
        QJsonObject{{QStringLiteral("id"), tabId},
                    {QStringLiteral("settings"),
                     QJsonObject{{QStringLiteral("mode"),
                                  QStringLiteral("disabled")}}}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    reply = backend->invoke(
        QStringLiteral("tabs"), QStringLiteral("getZoomSettings"),
        QJsonObject{{QStringLiteral("id"), tabId}});
    QCOMPARE(reply.value(QLatin1String("result")).toObject()
                 .value(QLatin1String("mode")).toString(),
             QStringLiteral("disabled"));
    // 'disabled' refuses the extension's own setZoom (Chrome parity).
    QCOMPARE(backend->invoke(
                 QStringLiteral("tabs"), QStringLiteral("setZoom"),
                 QJsonObject{{QStringLiteral("id"), tabId},
                             {QStringLiteral("zoom"), 2.0}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("zoom-disabled"));
    // A bogus enum is a typed refusal — the record keeps mode only.
    QCOMPARE(backend->invoke(
                 QStringLiteral("tabs"),
                 QStringLiteral("setZoomSettings"),
                 QJsonObject{{QStringLiteral("id"), tabId},
                             {QStringLiteral("settings"),
                              QJsonObject{{QStringLiteral("scope"),
                                           QStringLiteral("bogus")}}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));

    // --- captureVisibleTab --------------------------------------------
    // Covered: the fixture's 'tabs' grant sees every tab url, so the
    // grab leaves as a png data url.
    reply = backend->invoke(
        QStringLiteral("tabs"), QStringLiteral("captureVisibleTab"),
        QJsonObject{{QStringLiteral("windowId"), wid}});
    QVERIFY2(reply.value(QLatin1String("ok")).toBool(),
             qPrintable(reply.value(QLatin1String("error")).toString()));
    const QString dataUrl =
        reply.value(QLatin1String("result")).toString();
    QVERIFY(dataUrl.startsWith(
        QStringLiteral("data:image/png;base64,")));
    QVERIFY(dataUrl.size() > 64);
    // An explicit windowId resolving nowhere is a typed refusal.
    QCOMPARE(backend->invoke(
                 QStringLiteral("tabs"),
                 QStringLiteral("captureVisibleTab"),
                 QJsonObject{{QStringLiteral("windowId"), 424242}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("no-window"));
    // Uncovered: an id the runtime never granted 'tabs'/host
    // coverage — the seesUrls gate refuses before pixels leave.
    ExtensionShimBackend uncovered(
        QStringLiteral("unregisteredzoomid00"),
        QStringLiteral("tst-zoom-uncovered"), dir);
    QCOMPARE(uncovered.invoke(
                 QStringLiteral("tabs"),
                 QStringLiteral("captureVisibleTab"),
                 QJsonObject{{QStringLiteral("windowId"), wid}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("no-permission"));

    // deleteLater, not close(): multiple open tabs trip the
    // confirmClosingMultipleTabs QMessageBox — modal, unanswerable.
    window->deleteLater();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    manager->setExtensionEnabled(extId, false);
    QTRY_VERIFY_WITH_TIMEOUT(!runtime->host(extId), 20000);
    manager->removeExtension(extId);
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

    // SEC26: a still-pending mid must not re-send — a duplicate would
    // overwrite the pending entry and orphan the first send's timer.
    const QString m5 = QStringLiteral("relayext0000000001#5#t");
    QCOMPARE(runtime->relaySend(&bg, m5, QJsonObject()), 1);
    QCOMPARE(popupEvents.size(), 5);
    QCOMPARE(runtime->relaySend(&bg, m5, QJsonObject()), 0);
    QCOMPARE(popupEvents.size(), 5);   // no second fan-out

    // SEC26: portConnect dedups an already-registered end — a page
    // re-firing connect must not grow the chan's end list.
    QCOMPARE(runtime->relayPortConnect(
                 &bg, chan, QStringLiteral("p1b"),
                 QString(), QString()), 1);
    QCOMPARE(runtime->relayPortConnect(
                 &bg, chan, QStringLiteral("p1b"),
                 QString(), QString()), 0);

    // SEC26: sender.url is host-verified — the bridge's context page
    // answers it, never the caller-supplied senderUrl.  A bridge with
    // no page keeps the arg (tests and diagnostics only).
    auto *contextView = new QWebEngineView;
    auto *contextPage = new QWebEnginePage(contextView);
    contextView->setPage(contextPage);
    contextPage->load(QUrl(QStringLiteral("about:blank")));
    QTRY_VERIFY_WITH_TIMEOUT(
        contextPage->url() == QUrl(QStringLiteral("about:blank")),
        10000);
    bg.setContextPage(contextPage);
    const QString chan2 = QStringLiteral("relayext0000000001#p2");
    QCOMPARE(runtime->relayPortConnect(
                 &bg, chan2, QStringLiteral("p2"),
                 QString(),
                 QStringLiteral("https://forged.example/evil")), 1);
    QCOMPARE(popupEvents.last().at(0).toString(),
             QStringLiteral("__arktos.portConnect"));
    QCOMPARE(popupEvents.last().at(1).toJsonObject()
             .value(QLatin1String("sender")).toObject()
             .value(QLatin1String("url")).toString(),
             QStringLiteral("about:blank"));
    contextView->deleteLater();
    bg.setContextPage(nullptr);

    // Teardown unregisters; a further send finds no peers.
    runtime->unregisterRelayContext(&popup);
    QCOMPARE(runtime->relaySend(
                 &bg, m4, QJsonObject()), 0);
    runtime->unregisterRelayContext(&bg);
    runtime->unregisterRelayContext(&other);

    QDir(dir).removeRecursively();
}

// SEC26: the extension-chrome navigation lock — the action popup
// and the health probe carry the ER20 JavascriptEnabled lift, so
// their main frame may only ever hold the extension's own origin
// (or about:blank); foreign web content and other extensions'
// documents are refused, and subframes stay inside the origin too.
void tst_ExtensionManager::extensionChromeNavLock()
{
    const QString id = QStringLiteral("chromeext00000000000000001");
    using NP = ExtensionChromePage;
    // Main frame: own origin + about:blank pass.
    QVERIFY(NP::navigationAllowed(
        QUrl(QStringLiteral("chrome-extension://") + id
             + QStringLiteral("/popup.html")),
        id, true));
    QVERIFY(NP::navigationAllowed(
        QUrl(QStringLiteral("chrome-extension://") + id
             + QStringLiteral("/manifest.json#arktos-healthprobe")),
        id, true));
    QVERIFY(NP::navigationAllowed(
        QUrl(QStringLiteral("about:blank")), id, true));
    // Main frame: foreign web content and a DIFFERENT extension's
    // document are refused.
    QVERIFY(!NP::navigationAllowed(
        QUrl(QStringLiteral("https://forged.example/evil")),
        id, true));
    QVERIFY(!NP::navigationAllowed(
        QUrl(QStringLiteral("file:///etc/passwd")), id, true));
    QVERIFY(!NP::navigationAllowed(
        QUrl(QStringLiteral(
            "chrome-extension://otherext000000000000002/x.html")),
        id, true));
    QVERIFY(!NP::navigationAllowed(
        QUrl(QStringLiteral("javascript:alert(1)")), id, true));
    QVERIFY(!NP::navigationAllowed(
        QUrl(QStringLiteral("data:text/html,<b>x</b>")), id, true));
    // Subframes: only the extension's own origin — no about:blank
    // exemption and no foreign content.
    QVERIFY(NP::navigationAllowed(
        QUrl(QStringLiteral("chrome-extension://") + id
             + QStringLiteral("/frame.html")),
        id, false));
    QVERIFY(!NP::navigationAllowed(
        QUrl(QStringLiteral("https://tracker.example/px")),
        id, false));
    QVERIFY(!NP::navigationAllowed(
        QUrl(QStringLiteral("about:blank")), id, false));
    QVERIFY(!NP::navigationAllowed(
        QUrl(QStringLiteral(
            "chrome-extension://otherext000000000000002/f.html")),
        id, false));
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
        // ER16: B also carries 'contextMenus' — it must RECEIVE the
        // click event (the namespace gate passes) but the OnClickData
        // pageUrl/tab url scrub still applies: no 'tabs' grant and no
        // host permission means url-bearing fields leave stripped.
        f.write("{\"manifest_version\":3,\"name\":\"tst-events-b\","
                "\"version\":\"1.0\",\"permissions\":[\"storage\","
                "\"contextMenus\"],"
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
    // A's 'tabs' grant keeps the OnClickData pageUrl + tab url.
    QCOMPARE(eval(runtime->host(idA)->page(), QStringLiteral(
        "(self.__extEvents||[]).some(function(e){"
        " return e.n==='contextMenus.onClicked'&&e.a[0]"
        "   &&e.a[0].pageUrl!==undefined&&e.a[1]"
        "   &&e.a[1].url!==undefined;})")).toBool(), true);

    // --- ER16: B holds 'contextMenus' but neither 'tabs' nor a host
    // grant — the click reaches it with every url-bearing field
    // scrubbed (OnClickData.pageUrl, the tab arg's url).
    QVERIFY(backendB->invoke(
        QStringLiteral("contextMenus"), QStringLiteral("create"),
        QJsonObject{{QStringLiteral("props"),
                     QJsonObject{{QStringLiteral("id"),
                                  QStringLiteral("miB")},
                                 {QStringLiteral("title"),
                                  QStringLiteral("B Item")},
                                 {QStringLiteral("contexts"),
                                  QJsonArray{QStringLiteral(
                                      "page")}}}}})
        .value(QLatin1String("ok")).toBool());
    ExtensionEventHub::deliverMenuClicked(idB, QStringLiteral("miB"),
                                          cinfo, view);
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(runtime->host(idB)->page(), QStringLiteral(
            "(self.__extEvents||[]).some(function(e){"
            " return e.n==='contextMenus.onClicked'&&e.a[0]"
            "   &&e.a[0].menuItemId==='miB';})")).toBool(),
        20000);
    QCOMPARE(eval(runtime->host(idB)->page(), QStringLiteral(
        "(self.__extEvents||[]).some(function(e){"
        " return e.n==='contextMenus.onClicked'&&e.a[0]"
        "   &&e.a[0].pageUrl===undefined&&e.a[1]"
        "   &&e.a[1].url===undefined;})")).toBool(), true);

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

// ER12 — chrome.notifications end to end through a hosted
// background: the fixture's service worker registers all four event
// listeners and creates a notification; the leg inspects the record
// via getAll, then drives the daemon-signal handlers directly (the
// ARKTOS_EXTNOTIFY_STUB presenter supplies the platform id so no
// desktop banner is needed) and asserts the click/close events reach
// the page listeners.  A second fixture without the 'notifications'
// permission collects the typed denial.
void tst_ExtensionManager::extensionNotifications()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");
    qputenv("ARKTOS_EXTNOTIFY_STUB", "1");

    const QString swA = QStringLiteral(
        "self.__ntf=[];\n"
        "chrome.notifications.onClicked.addListener(function(id){\n"
        " self.__ntf.push(['clicked',id]);});\n"
        "chrome.notifications.onButtonClicked.addListener(\n"
        " function(id,i){self.__ntf.push(['btn',id,i]);});\n"
        "chrome.notifications.onClosed.addListener(\n"
        " function(id,u){self.__ntf.push(['closed',id,u]);});\n"
        "chrome.notifications.onShown.addListener(function(id){\n"
        " self.__ntf.push(['shown',id]);});\n"
        "chrome.notifications.create('ntf1',{type:'basic',\n"
        " title:'Hello',message:'World',\n"
        " iconUrl:chrome.runtime.getURL('icon.png'),\n"
        " buttons:[{title:'Yes'},{title:'No'}]})\n"
        " .then(function(id){self.__ntf.push(['created',id]);})\n"
        " .catch(function(e){self.__ntf.push(['err',String(e)]);});\n");
    const QString swB = QStringLiteral(
        "chrome.notifications.create('np',{type:'basic',title:'t',"
        "message:'m'}).catch(function(){});\n"
        "chrome.notifications.create('bad',{type:'bogus',title:'t'})"
        ".catch(function(){});\n");

    const QString root = QDir::temp().filePath(
        QStringLiteral("tst-extntf-%1")
            .arg(QCoreApplication::applicationPid()));
    const QString dirA = root + QLatin1String("/tst-notify-a");
    const QString dirB = root + QLatin1String("/tst-notify-b");
    for (const QString &dir : {dirA, dirB}) {
        QVERIFY(QDir().mkpath(dir));
    }
    // A bundle-local icon — chrome-extension:// resolution must land
    // inside the extension dir.
    QFile icon(dirA + QLatin1String("/icon.png"));
    QVERIFY(icon.open(QIODevice::WriteOnly));
    icon.write("\x89PNG\r\n\x1a\n");
    icon.close();
    const auto writeFixture = [](const QString &dir,
                                 const QString &name,
                                 const QString &permissions,
                                 const QString &sw) {
        QFile manifestFile(dir + QLatin1String("/manifest.json"));
        if (!manifestFile.open(QIODevice::WriteOnly))
            return false;
        manifestFile.write("{\"manifest_version\":3,\"name\":\""
            + name.toUtf8() + "\",\"version\":\"1.0\",\"permissions\":["
            + permissions.toUtf8()
            + "],\"background\":{\"service_worker\":\"sw.js\"}}");
        manifestFile.close();
        QFile worker(dir + QLatin1String("/sw.js"));
        if (!worker.open(QIODevice::WriteOnly))
            return false;
        worker.write(sw.toUtf8());
        worker.close();
        return true;
    };
    QVERIFY(writeFixture(dirA, QStringLiteral("tst-notify-a"),
                         QStringLiteral("\"notifications\""), swA));
    QVERIFY(writeFixture(dirB, QStringLiteral("tst-notify-b"),
                         QStringLiteral("\"storage\""), swB));

    ExtensionManager *manager = ExtensionManager::instance();
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();
    ExtensionNotificationCenter *center =
        ExtensionNotificationCenter::instance();

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
    const QString idA = loadReady(dirA, QStringLiteral("tst-notify-a"));
    const QString idB = loadReady(dirB, QStringLiteral("tst-notify-b"));
    QVERIFY2(!idA.isEmpty() && !idB.isEmpty(),
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
    const auto saw = [&eval](QWebEnginePage *page,
                             const QString &predicate) {
        return eval(page, QStringLiteral(
            "(self.__ntf||[]).some(%1)").arg(predicate)).toBool();
    };

    // create() resolved with the caller's id and the stub presenter
    // acknowledged it (onShown) — the record is inspectable.
    QTRY_VERIFY_WITH_TIMEOUT(saw(runtime->host(idA)->page(),
        QStringLiteral("function(e){return e[0]==='created'"
                       "&&e[1]==='ntf1';}")), 30000);
    QTRY_VERIFY_WITH_TIMEOUT(saw(runtime->host(idA)->page(),
        QStringLiteral("function(e){return e[0]==='shown'"
                       "&&e[1]==='ntf1';}")), 30000);
    QVERIFY(center->presented(idA, QStringLiteral("ntf1")));
    QVERIFY(center->platformIdFor(idA, QStringLiteral("ntf1")) != 0);
    // getAll is inspectable — runJavaScript never awaits promises,
    // so the page records results into __ntf and the leg polls.
    eval(runtime->host(idA)->page(), QStringLiteral(
        "chrome.notifications.getAll().then(function(all){"
        "self.__ntf.push(['all',all&&all.ntf1&&all.ntf1.title,"
        "all&&all.ntf1&&all.ntf1.buttons.length]);})"));
    QTRY_VERIFY_WITH_TIMEOUT(saw(runtime->host(idA)->page(),
        QStringLiteral("function(e){return e[0]==='all'"
                       "&&e[1]==='Hello'&&e[2]===2;}")), 20000);

    // update() merges over the stored options.
    eval(runtime->host(idA)->page(), QStringLiteral(
        "chrome.notifications.update('ntf1',{type:'basic',"
        "message:'v2'}).then(function(v){self.__ntf.push("
        "['upd',v]);})"));
    QTRY_VERIFY_WITH_TIMEOUT(saw(runtime->host(idA)->page(),
        QStringLiteral("function(e){return e[0]==='upd'"
                       "&&e[1]===true;}")), 20000);
    eval(runtime->host(idA)->page(), QStringLiteral(
        "chrome.notifications.getAll().then(function(all){"
        "self.__ntf.push(['all2',all.ntf1.message,"
        "all.ntf1.title]);})"));
    QTRY_VERIFY_WITH_TIMEOUT(saw(runtime->host(idA)->page(),
        QStringLiteral("function(e){return e[0]==='all2'"
                       "&&e[1]==='v2'&&e[2]==='Hello';}")), 20000);
    // A missing id answers false — never silently ok.
    eval(runtime->host(idA)->page(), QStringLiteral(
        "chrome.notifications.update('gone',{type:'basic'})"
        ".then(function(v){self.__ntf.push(['updGone',v]);})"));
    QTRY_VERIFY_WITH_TIMEOUT(saw(runtime->host(idA)->page(),
        QStringLiteral("function(e){return e[0]==='updGone'"
                       "&&e[1]===false;}")), 20000);

    // Daemon-signal routing: 'default' is the body click, 'btn-N' the
    // button index — both reach the extension's listeners.  The
    // platform id is re-read here: update() re-presents, and a
    // presenter may return a fresh handle for the replaced banner.
    const quint32 pid =
        center->platformIdFor(idA, QStringLiteral("ntf1"));
    QVERIFY(pid != 0);
    center->onActionInvoked(pid, QStringLiteral("default"));
    QTRY_VERIFY_WITH_TIMEOUT(saw(runtime->host(idA)->page(),
        QStringLiteral("function(e){return e[0]==='clicked'"
                       "&&e[1]==='ntf1';}")), 20000);
    center->onActionInvoked(pid, QStringLiteral("btn-1"));
    QTRY_VERIFY_WITH_TIMEOUT(saw(runtime->host(idA)->page(),
        QStringLiteral("function(e){return e[0]==='btn'"
                       "&&e[1]==='ntf1'&&e[2]===1;}")), 20000);

    // A daemon close (reason 2 = dismissed by user) routes onClosed
    // with byUser=true and drops the record.
    center->onNotificationClosed(pid, 2);
    QTRY_VERIFY_WITH_TIMEOUT(saw(runtime->host(idA)->page(),
        QStringLiteral("function(e){return e[0]==='closed'"
                       "&&e[1]==='ntf1'&&e[2]===true;}")), 20000);
    eval(runtime->host(idA)->page(), QStringLiteral(
        "chrome.notifications.getAll().then(function(all){"
        "self.__ntf.push(['all3',!!all.ntf1]);})"));
    QTRY_VERIFY_WITH_TIMEOUT(saw(runtime->host(idA)->page(),
        QStringLiteral("function(e){return e[0]==='all3'"
                       "&&e[1]===false;}")), 20000);

    // A programmatic clear() reports onClosed(byUser=false).
    eval(runtime->host(idA)->page(), QStringLiteral(
        "chrome.notifications.create('ntf2',{type:'basic',"
        "title:'T',message:'M'}).then(function(id){"
        "self.__ntf.push(['created',id]);})"));
    QTRY_VERIFY_WITH_TIMEOUT(saw(runtime->host(idA)->page(),
        QStringLiteral("function(e){return e[0]==='created'"
                       "&&e[1]==='ntf2';}")), 20000);
    eval(runtime->host(idA)->page(), QStringLiteral(
        "chrome.notifications.clear('ntf2').then(function(v){"
        "self.__ntf.push(['clr',v]);})"));
    QTRY_VERIFY_WITH_TIMEOUT(saw(runtime->host(idA)->page(),
        QStringLiteral("function(e){return e[0]==='clr'"
                       "&&e[1]===true;}")), 20000);
    QTRY_VERIFY_WITH_TIMEOUT(saw(runtime->host(idA)->page(),
        QStringLiteral("function(e){return e[0]==='closed'"
                       "&&e[1]==='ntf2'&&e[2]===false;}")), 20000);
    eval(runtime->host(idA)->page(), QStringLiteral(
        "chrome.notifications.clear('ntf2').then(function(v){"
        "self.__ntf.push(['clr2',v]);})"));
    QTRY_VERIFY_WITH_TIMEOUT(saw(runtime->host(idA)->page(),
        QStringLiteral("function(e){return e[0]==='clr2'"
                       "&&e[1]===false;}")), 20000);

    // B never got the permission — its creates are typed denials,
    // health-visible.  'bad' (a bogus type on a permission-less
    // caller) is the same no-permission refusal.
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime->host(idB)->shimGaps().contains(QStringLiteral(
            "chrome.notifications.create: no-permission")),
        20000);

    // Teardown drops the extension's records like Chrome's unload.
    manager->setExtensionEnabled(idA, false);
    manager->setExtensionEnabled(idB, false);
    QTRY_VERIFY_WITH_TIMEOUT(
        !runtime->host(idA) && !runtime->host(idB), 20000);
    manager->removeExtension(idA);
    manager->removeExtension(idB);
    QDir(root).removeRecursively();
}

// ER13 — chrome.idle over the hosted runtime.  Fixture A declares
// 'idle' and drives the whole surface: an onStateChanged listener
// (which arms the watcher through the shim's subscribe op), a 1s
// detection interval, a per-call queryState threshold, a negative
// argument and the ChromeOS-only getAutoLockDelay.  The leg sends a
// real QKeyEvent through the application object — the watcher's
// app-wide event filter is the path under test — and watches the
// active -> idle -> active transitions cross into the hosted page.
// Fixture B has no 'idle' permission: every call is a typed
// no-permission denial and the state fan-out never reaches it.
void tst_ExtensionManager::extensionIdle()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");
    // Chrome's 15s detection floor would make the leg sleep 16s+;
    // the watcher honors an explicit override for exactly this.
    qputenv("ARKTOS_EXT_IDLE_MIN_S", "1");

    const QString swA = QStringLiteral(
        "self.__idle=[];\n"
        "chrome.idle.onStateChanged.addListener(function(s){\n"
        " self.__idle.push(['state',s]);});\n"
        "chrome.idle.setDetectionInterval(1);\n"
        "chrome.idle.queryState(-2).catch(function(){});\n"
        "chrome.idle.getAutoLockDelay().catch(function(){});\n");
    const QString swB = QStringLiteral(
        "self.__idle=[];\n"
        "chrome.idle.onStateChanged.addListener(function(s){\n"
        " self.__idle.push(['state',s]);});\n"
        "chrome.idle.queryState(15).catch(function(){});\n");

    const QString root = QDir::temp().filePath(
        QStringLiteral("tst-extidle-%1")
            .arg(QCoreApplication::applicationPid()));
    const QString dirA = root + QLatin1String("/tst-idle-a");
    const QString dirB = root + QLatin1String("/tst-idle-b");
    for (const QString &dir : {dirA, dirB})
        QVERIFY(QDir().mkpath(dir));
    const auto writeFixture = [](const QString &dir,
                                 const QString &name,
                                 const QString &permissions,
                                 const QString &sw) {
        QFile manifestFile(dir + QLatin1String("/manifest.json"));
        if (!manifestFile.open(QIODevice::WriteOnly))
            return false;
        manifestFile.write("{\"manifest_version\":3,\"name\":\""
            + name.toUtf8() + "\",\"version\":\"1.0\",\"permissions\":["
            + permissions.toUtf8()
            + "],\"background\":{\"service_worker\":\"sw.js\"}}");
        manifestFile.close();
        QFile worker(dir + QLatin1String("/sw.js"));
        if (!worker.open(QIODevice::WriteOnly))
            return false;
        worker.write(sw.toUtf8());
        worker.close();
        return true;
    };
    QVERIFY(writeFixture(dirA, QStringLiteral("tst-idle-a"),
                         QStringLiteral("\"idle\""), swA));
    QVERIFY(writeFixture(dirB, QStringLiteral("tst-idle-b"),
                         QStringLiteral("\"storage\""), swB));

    ExtensionManager *manager = ExtensionManager::instance();
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();
    ExtensionIdleWatcher *watcher = ExtensionIdleWatcher::instance();

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
    const QString idA = loadReady(dirA, QStringLiteral("tst-idle-a"));
    const QString idB = loadReady(dirB, QStringLiteral("tst-idle-b"));
    QVERIFY2(!idA.isEmpty() && !idB.isEmpty(),
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
    const auto saw = [&eval](QWebEnginePage *page,
                             const QString &predicate) {
        return eval(page, QStringLiteral(
            "(self.__idle||[]).some(%1)").arg(predicate)).toBool();
    };
    const auto sendInput = []() {
        // A real key event through the application object — the
        // app-wide event filter is the production path under test.
        QKeyEvent press(QEvent::KeyPress, Qt::Key_A,
                        Qt::NoModifier);
        QCoreApplication::sendEvent(qApp, &press);
    };

    // The listener's subscribe armed the watcher; the 1s interval
    // (clamped at the test floor) drops it to idle with no input.
    QTRY_VERIFY_WITH_TIMEOUT(watcher->armed(), 20000);
    QTRY_VERIFY_WITH_TIMEOUT(
        watcher->state() == QLatin1String("idle"), 20000);
    QTRY_VERIFY_WITH_TIMEOUT(saw(runtime->host(idA)->page(),
        QStringLiteral("function(e){return e[0]==='state'"
                       "&&e[1]==='idle';}")), 20000);

    // Synthetic input flips it back — the active transition reaches
    // the listener and queryState answers active at its own
    // threshold.
    sendInput();
    QTRY_VERIFY_WITH_TIMEOUT(saw(runtime->host(idA)->page(),
        QStringLiteral("function(e){return e[0]==='state'"
                       "&&e[1]==='active';}")), 20000);
    eval(runtime->host(idA)->page(), QStringLiteral(
        "chrome.idle.queryState(1).then(function(s){"
        "self.__idle.push(['q',s]);});'ok'"));
    QTRY_VERIFY_WITH_TIMEOUT(saw(runtime->host(idA)->page(),
        QStringLiteral("function(e){return e[0]==='q'"
                       "&&e[1]==='active';}")), 20000);

    // Input stopped: the boundary re-fires and the per-call
    // threshold is independent of the declared state — a long
    // threshold still answers active (Chrome semantics).
    QTRY_VERIFY_WITH_TIMEOUT(
        watcher->state() == QLatin1String("idle"), 20000);
    eval(runtime->host(idA)->page(), QStringLiteral(
        "chrome.idle.queryState(1).then(function(s){"
        "self.__idle.push(['q1',s]);});'ok'"));
    eval(runtime->host(idA)->page(), QStringLiteral(
        "chrome.idle.queryState(3600).then(function(s){"
        "self.__idle.push(['q3600',s]);});'ok'"));
    const auto waitFor = [&eval](QWebEnginePage *page,
                                 const QString &predicate, int ms) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < ms) {
            if (eval(page, QStringLiteral(
                    "(self.__idle||[]).some(%1)")
                    .arg(predicate)).toBool())
                return true;
            QTest::qWait(50);
        }
        return false;
    };
    const auto idleDump = [&eval](QWebEnginePage *page) {
        return eval(page, QStringLiteral(
            "JSON.stringify(self.__idle||[])")).toString();
    };
    QVERIFY2(waitFor(runtime->host(idA)->page(),
        QStringLiteral("function(e){return e[0]==='q1';}"), 20000),
        qPrintable(QStringLiteral("idle ledger: %1")
            .arg(idleDump(runtime->host(idA)->page()))));
    QVERIFY2(waitFor(runtime->host(idA)->page(),
        QStringLiteral("function(e){return e[0]==='q3600';}"), 20000),
        qPrintable(QStringLiteral("idle ledger: %1")
            .arg(idleDump(runtime->host(idA)->page()))));
    QTRY_VERIFY_WITH_TIMEOUT(saw(runtime->host(idA)->page(),
        QStringLiteral("function(e){return e[0]==='q1'"
                       "&&e[1]==='idle';}")), 20000);
    QTRY_VERIFY_WITH_TIMEOUT(saw(runtime->host(idA)->page(),
        QStringLiteral("function(e){return e[0]==='q3600'"
                       "&&e[1]==='active';}")), 20000);
    QTRY_VERIFY_WITH_TIMEOUT(saw(runtime->host(idA)->page(),
        QStringLiteral(
            "function(e){return e[0]==='state'&&e[1]==='idle'"
            "&&self.__idle.filter(function(x){return "
            "x[0]==='state';}).length>=2;}")), 20000);

    // Typed failures are ledgered, never silent.
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime->host(idA)->shimGaps().contains(QStringLiteral(
            "chrome.idle.queryState: invalid-argument")), 20000);
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime->host(idA)->shimGaps().contains(QStringLiteral(
            "chrome.idle.getAutoLockDelay: unsupported")), 20000);

    // B holds no 'idle' permission: calls are denied and the state
    // fan-out is permission-gated away from its listener — a fresh
    // transition right now must still never reach it.
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime->host(idB)->shimGaps().contains(QStringLiteral(
            "chrome.idle.queryState: no-permission")), 20000);
    sendInput();
    QTRY_VERIFY_WITH_TIMEOUT(
        watcher->state() == QLatin1String("active"), 20000);
    QTest::qWait(500);
    QVERIFY(!saw(runtime->host(idB)->page(),
               QStringLiteral("function(e){return e[0]==='state';}")));

    manager->setExtensionEnabled(idA, false);
    manager->setExtensionEnabled(idB, false);
    QTRY_VERIFY_WITH_TIMEOUT(
        !runtime->host(idA) && !runtime->host(idB), 20000);
    manager->removeExtension(idA);
    manager->removeExtension(idB);
    QDir(root).removeRecursively();
}

// ER14 — chrome.commands.  Two hosted fixtures declare manifest
// commands: A carries _execute_action WITH a popup (the key press
// must open the real action popup, never reach onCommand) plus a
// plain command, an unbound one and an unbindable one; B's action
// has no popup, so its _execute_action must fire action.onClicked
// instead.  The leg drives real QShortcut activations through
// QTest key events on a real BrowserMainWindow and checks the
// Chrome-shaped (command, tab) dispatch into the hosted pages.
void tst_ExtensionManager::extensionCommands()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");

    const QString sw = QStringLiteral(
        "self.__cmd=[];\n"
        "chrome.commands.onCommand.addListener(function(c,t){\n"
        " self.__cmd.push(['onCommand',c,"
        "  t===null||t===undefined?null:t.id]);});\n"
        "chrome.action.onClicked.addListener(function(t){\n"
        " self.__cmd.push(['actionClicked',"
        "  t===null||t===undefined?null:t.id]);});\n");

    const QString root = QDir::temp().filePath(
        QStringLiteral("tst-extcmd-%1")
            .arg(QCoreApplication::applicationPid()));
    const QString dirA = root + QLatin1String("/tst-cmd-a");
    const QString dirB = root + QLatin1String("/tst-cmd-b");
    for (const QString &dir : {dirA, dirB})
        QVERIFY(QDir().mkpath(dir));
    const auto writeFile = [](const QString &path,
                              const QByteArray &body) {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return false;
        f.write(body);
        f.close();
        return true;
    };
    // A: _execute_action + popup, one bound command, one declared-
    // without-a-key command and one unbindable media key.
    QVERIFY(writeFile(dirA + QLatin1String("/manifest.json"),
        "{\"manifest_version\":3,\"name\":\"tst-cmd-a\","
        "\"version\":\"1.0\",\"permissions\":[\"storage\"],"
        "\"action\":{\"default_title\":\"Cmd A\","
        "\"default_popup\":\"popup.html\"},"
        "\"commands\":{"
        "\"_execute_action\":{\"suggested_key\":"
        "{\"default\":\"Ctrl+Shift+Y\"},\"description\":\"open\"},"
        "\"do_thing\":{\"suggested_key\":{\"default\":"
        "\"Ctrl+Shift+U\"},\"description\":\"thing\"},"
        "\"unbound_thing\":{\"description\":\"no key\"},"
        "\"bad_key\":{\"suggested_key\":{\"default\":"
        "\"MediaNextTrack\"}}},"
        "\"background\":{\"service_worker\":\"sw.js\"}}"));
    QVERIFY(writeFile(dirA + QLatin1String("/sw.js"), sw.toUtf8()));
    QVERIFY(writeFile(dirA + QLatin1String("/popup.html"),
        "<!doctype html><title>a</title>"));
    // B: action with NO popup — _execute_action fires
    // action.onClicked; plus one ordinary bound command.
    QVERIFY(writeFile(dirB + QLatin1String("/manifest.json"),
        "{\"manifest_version\":3,\"name\":\"tst-cmd-b\","
        "\"version\":\"1.0\",\"permissions\":[\"storage\"],"
        "\"action\":{\"default_title\":\"Cmd B\"},"
        "\"commands\":{"
        "\"_execute_action\":{\"suggested_key\":"
        "{\"default\":\"Ctrl+Shift+8\"}},"
        "\"plain_thing\":{\"suggested_key\":{\"default\":"
        "\"Ctrl+Shift+9\"}}},"
        "\"background\":{\"service_worker\":\"sw.js\"}}"));
    QVERIFY(writeFile(dirB + QLatin1String("/sw.js"), sw.toUtf8()));
    // Only referenced through a runtime chrome.action.setPopup —
    // the manifest declares no popup for B.
    QVERIFY(writeFile(dirB + QLatin1String("/popup.html"),
        "<!doctype html><title>b</title>"));

    ExtensionManager *manager = ExtensionManager::instance();
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();
    ExtensionCommandRegistry *registry =
        ExtensionCommandRegistry::instance();

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
    const QString idA = loadReady(dirA, QStringLiteral("tst-cmd-a"));
    const QString idB = loadReady(dirB, QStringLiteral("tst-cmd-b"));
    QVERIFY2(!idA.isEmpty() && !idB.isEmpty(),
             "fixtures loaded + hosted");

    // The sync pass parsed both manifests — every key-bearing
    // command is bound, the no-key and unbindable ones are listed
    // as unbound (they still appear in getAll, like Chrome's
    // unassigned rows).
    QTRY_VERIFY_WITH_TIMEOUT(
        registry->boundCommands(idA).contains(
            QStringLiteral("_execute_action"))
            && registry->boundCommands(idA).contains(
                QStringLiteral("do_thing")), 20000);
    QVERIFY(registry->unboundCommands(idA).contains(
        QStringLiteral("unbound_thing")));
    QVERIFY(registry->unboundCommands(idA).contains(
        QStringLiteral("bad_key")));
    QVERIFY(registry->boundCommands(idB).contains(
        QStringLiteral("_execute_action")));
    QVERIFY(registry->boundCommands(idB).contains(
        QStringLiteral("plain_thing")));

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

    // getAll answers from the manifest the handshake shipped —
    // unbound commands list an empty shortcut exactly like Chrome.
    eval(runtime->host(idA)->page(), QStringLiteral(
        "chrome.commands.getAll().then(function(c){"
        "self.__all=c;});'ok'"));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(runtime->host(idA)->page(), QStringLiteral(
            "(self.__all||[]).some(function(c){"
            "return c.name==='do_thing'"
            "&&c.shortcut==='Ctrl+Shift+U';})")).toBool(), 20000);
    QVERIFY(eval(runtime->host(idA)->page(), QStringLiteral(
        "(self.__all||[]).some(function(c){"
        "return c.name==='unbound_thing'&&c.shortcut==='';})"))
        .toBool());
    // An unbindable suggestion also reports '' — the declared key
    // is not parroted as assigned.
    QVERIFY(eval(runtime->host(idA)->page(), QStringLiteral(
        "(self.__all||[]).some(function(c){"
        "return c.name==='bad_key'&&c.shortcut==='';})")).toBool());

    // A real window: the registry arms it (event filter or the next
    // sync pass) with one WindowShortcut per bound command.
    BrowserMainWindow *window = new BrowserMainWindow;
    window->show();
    window->activateWindow();
    runtime->syncNow();
    QTRY_VERIFY_WITH_TIMEOUT(registry->shortcutCount() >= 4, 20000);
    // QShortcut context matching resolves against the ACTIVE window —
    // under offscreen the activation lands asynchronously, so the key
    // press must wait for it or the window's shortcuts stay inert.
    QVERIFY(QTest::qWaitForWindowActive(window));

    // Ctrl+Shift+U -> commands.onCommand('do_thing', tab) — the
    // window's live tab rides as the second argument.
    QTest::keyClick(window, Qt::Key_U,
                    Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(runtime->host(idA)->page(), QStringLiteral(
            "(self.__cmd||[]).some(function(e){"
            "return e[0]==='onCommand'&&e[1]==='do_thing'"
            "&&typeof e[2]==='number';})")).toBool(), 20000);

    // B's own command reaches ONLY B — targeted delivery, never a
    // broadcast.
    QTest::keyClick(window, Qt::Key_9,
                    Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(runtime->host(idB)->page(), QStringLiteral(
            "(self.__cmd||[]).some(function(e){"
            "return e[0]==='onCommand'&&e[1]==='plain_thing'"
            "&&typeof e[2]==='number';})")).toBool(), 20000);
    QTest::qWait(500);
    QVERIFY(!eval(runtime->host(idA)->page(), QStringLiteral(
        "(self.__cmd||[]).some(function(e){"
        "return e[1]==='plain_thing';})")).toBool());

    // _execute_action on a popup-bearing action opens the real
    // action popup — Chrome never fires onCommand for it.
    ExtensionActionBar *bar =
        window->findChild<ExtensionActionBar *>();
    QVERIFY(bar);
    QTest::keyClick(window, Qt::Key_Y,
                    Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_VERIFY_WITH_TIMEOUT(
        bar->findChild<QFrame *>(
            QLatin1String("extensionActionPopup")) != nullptr,
        20000);
    QTest::qWait(500);
    QVERIFY(!eval(runtime->host(idA)->page(), QStringLiteral(
        "(self.__cmd||[]).some(function(e){"
        "return e[0]==='actionClicked'"
        "||e[1]==='_execute_action';})")).toBool());
    // The open bubble owns the active-window context — the window's
    // shortcuts go quiet while it is up, exactly like Chrome.
    if (QFrame *popup = bar->findChild<QFrame *>(
            QLatin1String("extensionActionPopup")))
        popup->close();
    QTRY_VERIFY_WITH_TIMEOUT(
        bar->findChild<QFrame *>(
            QLatin1String("extensionActionPopup")) == nullptr,
        20000);
    window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(window));

    // B's action declares no popup — its _execute_action fires
    // action.onClicked(tab) into the host instead.
    QTest::keyClick(window, Qt::Key_8,
                    Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(runtime->host(idB)->page(), QStringLiteral(
            "(self.__cmd||[]).some(function(e){"
            "return e[0]==='actionClicked'"
            "&&typeof e[1]==='number';})")).toBool(), 20000);

    // chrome.action.setPopup rewrites what _execute_action means at
    // runtime: a popup set on B's popup-less action makes the same
    // binding open the bubble (bar-anchored — B has no button);
    // clearing it returns the binding to action.onClicked.
    eval(runtime->host(idB)->page(), QStringLiteral(
        "chrome.action.setPopup({popup:'popup.html'})"
        ".then(function(){self.__popset=1;});'ok'"));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(runtime->host(idB)->page(), QStringLiteral(
            "self.__popset===1")).toBool(), 20000);
    const int clicksBefore = eval(runtime->host(idB)->page(),
        QStringLiteral(
            "(self.__cmd||[]).filter(function(e){"
            "return e[0]==='actionClicked';}).length")).toInt();
    QTest::keyClick(window, Qt::Key_8,
                    Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_VERIFY_WITH_TIMEOUT(
        bar->findChild<QFrame *>(
            QLatin1String("extensionActionPopup")) != nullptr,
        20000);
    QTest::qWait(500);
    QCOMPARE(eval(runtime->host(idB)->page(), QStringLiteral(
        "(self.__cmd||[]).filter(function(e){"
        "return e[0]==='actionClicked';}).length")).toInt(),
        clicksBefore);
    if (QFrame *popup = bar->findChild<QFrame *>(
            QLatin1String("extensionActionPopup")))
        popup->close();
    QTRY_VERIFY_WITH_TIMEOUT(
        bar->findChild<QFrame *>(
            QLatin1String("extensionActionPopup")) == nullptr,
        20000);
    window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(window));
    eval(runtime->host(idB)->page(), QStringLiteral(
        "chrome.action.setPopup({popup:''})"
        ".then(function(){self.__popclr=1;});'ok'"));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(runtime->host(idB)->page(), QStringLiteral(
            "self.__popclr===1")).toBool(), 20000);
    QTest::keyClick(window, Qt::Key_8,
                    Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(runtime->host(idB)->page(), QStringLiteral(
            "(self.__cmd||[]).filter(function(e){"
            "return e[0]==='actionClicked';}).length")).toInt()
                > clicksBefore, 20000);

    // The unbindable declaration produced a health-surface note,
    // not a silent drop.
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime->host(idA)->bridge()->backend()->notes().contains(
            QStringLiteral("commands.bad_key: suggested_key "
                           "'MediaNextTrack' is not bindable")),
        20000);

    // Disable tears the bindings down — the key press stops
    // dispatching instead of reaching a dead extension.
    manager->setExtensionEnabled(idA, false);
    QTRY_VERIFY_WITH_TIMEOUT(
        registry->boundCommands(idA).isEmpty(), 20000);
    const int before = eval(runtime->host(idB)->page(), QStringLiteral(
        "(self.__cmd||[]).length")).toInt();
    QTest::keyClick(window, Qt::Key_U,
                    Qt::ControlModifier | Qt::ShiftModifier);
    QTest::qWait(500);
    QCOMPARE(eval(runtime->host(idB)->page(), QStringLiteral(
        "(self.__cmd||[]).length")).toInt(), before);

    delete window;
    manager->setExtensionEnabled(idB, false);
    QTRY_VERIFY_WITH_TIMEOUT(!runtime->host(idB), 20000);
    manager->removeExtension(idA);
    manager->removeExtension(idB);
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

// MD12 — chrome.tabs.connect: a long-lived Port from the hosted
// context into a tab's page agent over the same relay machinery
// ER02's sendMessage uses.  The leg drives the real API from the
// host page: the agent's runtime.onConnect fires with a Chrome-
// shaped sender, postMessage flows both ways, the connecting port's
// sender.tab is the remote tab, and disconnect propagates in both
// directions.  The C++ gates are direct-driven like the sendMessage
// legs.
void tst_ExtensionManager::extensionTabConnect()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");

    const QString dir = QDir::temp().filePath(
        QStringLiteral("tst-exttabconn-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dir));
    {
        QFile f(dir + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-tabconn\","
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
            "window.__csConns=[];window.__csPorts=[];"
            "window.__csDisc=0;\n"
            "chrome.runtime.onConnect.addListener(function(p){"
            " window.__csConns.push({name:p.name,"
            "  id:(p.sender&&p.sender.id)||null,"
            "  origin:(p.sender&&p.sender.origin)||null,"
            "  hasTab:!!(p.sender&&p.sender.tab)});"
            " window.__csPorts.push(p);"
            " p.onMessage.addListener(function(m,port){"
            "  port.postMessage({echo:m});});"
            " p.onDisconnect.addListener(function(){"
            "  window.__csDisc++;});"
            "});\n");
        c.close();
    }

    ExtensionManager *manager = ExtensionManager::instance();
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();
    auto loaded = std::make_shared<QString>();
    QObject::connect(manager, &ExtensionManager::extensionLoaded,
                     this, [loaded](
                         const ExtensionManager::ExtensionInfo &info) {
        if (info.name == QLatin1String("tst-tabconn")
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
                "<html><body>tabconn</body></html>";
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
    view->load(QUrl(base + QStringLiteral("/connect")));
    QTRY_VERIFY_WITH_TIMEOUT(loadSpy.count() >= 1, 30000);
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(view->page(), probe + QStringLiteral(".bridgeUp"),
             agentWorld).toBool(),
        30000);
    const int tabId = ExtensionShimBackend::tabIdFor(view);
    QVERIFY(tabId > 0);

    // --- open the port from the hosted context ---------------------
    eval(host->page(), QStringLiteral(
        "self.__tc={msgs:[],disc:0};"
        "var p=chrome.tabs.connect(%1,{name:'md12-conn'});"
        "self.__tc.port=p;"
        "p.onMessage.addListener(function(m){"
        " self.__tc.msgs.push(m);});"
        "p.onDisconnect.addListener(function(){self.__tc.disc++;});"
        "0").arg(tabId));

    // The agent's runtime.onConnect fired with a Chrome-shaped
    // extension-context sender (id + extension origin, no .tab).
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(view->page(), QStringLiteral(
            "(window.__csConns||[]).length>=1"),
            agentWorld).toBool(),
        20000);
    const QJsonObject conn = QJsonDocument::fromJson(
        eval(view->page(), QStringLiteral(
            "JSON.stringify(window.__csConns[0])"),
            agentWorld).toString().toUtf8()).object();
    QCOMPARE(conn.value(QLatin1String("name")).toString(),
             QStringLiteral("md12-conn"));
    QCOMPARE(conn.value(QLatin1String("id")).toString(), id);
    QCOMPARE(conn.value(QLatin1String("origin")).toString(),
             QStringLiteral("chrome-extension://") + id);
    QCOMPARE(conn.value(QLatin1String("hasTab")).toBool(), false);

    // The connecting port's sender is the tab end — Chrome exposes
    // it as sender.tab once the offer resolves.
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(host->page(), QStringLiteral(
            "!!(self.__tc.port.sender&&self.__tc.port.sender.tab)"))
            .toBool(),
        20000);
    QCOMPARE(eval(host->page(), QStringLiteral(
                  "self.__tc.port.sender.tab.id")).toInt(), tabId);

    // --- messages both ways ----------------------------------------
    eval(host->page(), QStringLiteral(
        "self.__tc.port.postMessage({hello:1});0"));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(host->page(), QStringLiteral(
            "self.__tc.msgs.length>=1")).toBool(),
        20000);
    QCOMPARE(eval(host->page(), QStringLiteral(
                  "self.__tc.msgs[0].echo.hello")).toInt(), 1);
    // Page-initiated postMessage lands on the extension's onMessage.
    eval(view->page(), QStringLiteral(
        "window.__csPorts[0].postMessage({page:2});0"),
        agentWorld);
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(host->page(), QStringLiteral(
            "self.__tc.msgs.length>=2")).toBool(),
        20000);
    QCOMPARE(eval(host->page(), QStringLiteral(
                  "self.__tc.msgs[1].page")).toInt(), 2);

    // --- disconnect propagates page->extension ---------------------
    eval(view->page(), QStringLiteral(
        "window.__csPorts[0].disconnect();0"), agentWorld);
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(host->page(), QStringLiteral("self.__tc.disc>=1"))
            .toBool(),
        20000);

    // --- and extension->page on a second port -----------------------
    eval(host->page(), QStringLiteral(
        "self.__tc2={disc:0};"
        "var p=chrome.tabs.connect(%1,{name:'md12-conn2'});"
        "self.__tc2.port=p;"
        "p.onDisconnect.addListener(function(){self.__tc2.disc++;});"
        "0").arg(tabId));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(view->page(), QStringLiteral(
            "(window.__csConns||[]).length>=2"),
            agentWorld).toBool(),
        20000);
    eval(host->page(), QStringLiteral("self.__tc2.port.disconnect();0"));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(view->page(), QStringLiteral("window.__csDisc>=1"),
             agentWorld).toBool(),
        20000);

    // --- the C++ gates, direct-driven ------------------------------
    ChromeBridge *bridge = host->bridge();
    // Wrong tab id — the registry has no such view.
    QCOMPARE(bridge->invoke(
                 QStringLiteral("tabs"),
                 QStringLiteral("connect"),
                 QJsonObject{{QStringLiteral("id"), 424242},
                             {QStringLiteral("chan"),
                              id + QStringLiteral("#c#1")},
                             {QStringLiteral("portId"),
                              QStringLiteral("p1")}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("not-found"));
    // A chan outside the extension's namespace never routes.
    QCOMPARE(bridge->invoke(
                 QStringLiteral("tabs"),
                 QStringLiteral("connect"),
                 QJsonObject{{QStringLiteral("id"), tabId},
                             {QStringLiteral("chan"),
                              QStringLiteral("other#c#1")},
                             {QStringLiteral("portId"),
                              QStringLiteral("p1")}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    // Subframe addressing is the same documented gap sendMessage
    // answers — typed, not silently delivered to frame 0.
    QCOMPARE(bridge->invoke(
                 QStringLiteral("tabs"),
                 QStringLiteral("connect"),
                 QJsonObject{{QStringLiteral("id"), tabId},
                             {QStringLiteral("chan"),
                              id + QStringLiteral("#c#2")},
                             {QStringLiteral("portId"),
                              QStringLiteral("p2")},
                             {QStringLiteral("opts"),
                              QJsonObject{{QStringLiteral("frameId"),
                                           1}}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("unsupported"));

    // --- an uncovered page: the offer never happens -----------------
    WebView *view3 = tw->makeNewTab(false);
    QVERIFY(view3 && view3->page());
    QSignalSpy loadSpy3(view3->page(), &QWebEnginePage::loadFinished);
    view3->load(QUrl(QStringLiteral("qrc:/pip-player.html")));
    QTRY_VERIFY_WITH_TIMEOUT(loadSpy3.count() >= 1, 30000);
    const int tabId3 = ExtensionShimBackend::tabIdFor(view3);
    QCOMPARE(bridge->invoke(
                 QStringLiteral("tabs"),
                 QStringLiteral("connect"),
                 QJsonObject{{QStringLiteral("id"), tabId3},
                             {QStringLiteral("chan"),
                              id + QStringLiteral("#c#3")},
                             {QStringLiteral("portId"),
                              QStringLiteral("p3")}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("no-host-access"));

    // deleteLater, not close(): open tabs trip the modal
    // confirmClosingMultipleTabs prompt, unanswerable headless.
    window->deleteLater();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    manager->setExtensionEnabled(id, false);
    QTRY_VERIFY_WITH_TIMEOUT(!runtime->host(id), 20000);
    manager->removeExtension(id);
    QDir(dir).removeRecursively();
}

// MD13 — chrome.windows fidelity.  get/getCurrent/getLastFocused/
// getAll honour getInfo.populate with the seesUrls-scrubbed tabs
// array (and getInfo.windowTypes filters honestly — every Arktos
// window is "normal"); window.state reports the real Qt state;
// update moves/resizes the real widget and create honours the same
// bounds props; drawAttention routes through QApplication::alert.
void tst_ExtensionManager::extensionWindowsFidelity()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");

    const QString dir = QDir::temp().filePath(
        QStringLiteral("tst-extwin-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dir));
    {
        QFile f(dir + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-windows\","
                "\"version\":\"1.0\",\"permissions\":[\"tabs\"]}");
        f.close();
    }
    ExtensionShimBackend backend(
        QStringLiteral("tstmockextensionid0005"),
        QStringLiteral("tst-windows"), dir);

    BrowserMainWindow *window = new BrowserMainWindow;
    TabWidget *tw = window->tabWidget();
    QVERIFY(tw && tw->count() >= 1);
    WebView *extra = tw->makeNewTab(false);
    QVERIFY(extra);
    QCOMPARE(tw->count(), 2);
    const int wid = ExtensionShimBackend::windowIdFor(window);
    QVERIFY(wid > 0);

    const auto result = [](const QJsonObject &reply) {
        return reply.value(QLatin1String("result")).toObject();
    };

    // --- populate ----------------------------------------------------
    // get(getInfo.populate) returns the window's tabs array; the
    // unhosted backend holds no url coverage, so the QUAL03 scrub
    // strips url/title off every populated entry.
    QJsonObject reply = backend.invoke(
        QStringLiteral("windows"), QStringLiteral("get"),
        QJsonObject{{QStringLiteral("id"), wid},
                    {QStringLiteral("getInfo"),
                     QJsonObject{{QStringLiteral("populate"),
                                  true}}}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QJsonObject win = result(reply);
    QCOMPARE(win.value(QLatin1String("id")).toInt(), wid);
    QCOMPARE(win.value(QLatin1String("state")).toString(),
             QStringLiteral("normal"));
    QCOMPARE(win.value(QLatin1String("type")).toString(),
             QStringLiteral("normal"));
    QCOMPARE(win.value(QLatin1String("tabs")).toArray().size(),
             tw->count());
    const QJsonObject firstTab =
        win.value(QLatin1String("tabs")).toArray().first().toObject();
    QCOMPARE(firstTab.value(QLatin1String("windowId")).toInt(), wid);
    QVERIFY(!firstTab.contains(QLatin1String("url")));
    QVERIFY(firstTab.value(QLatin1String("id")).toInt() > 0);

    // populate defaults off — no tabs member at all.
    reply = backend.invoke(
        QStringLiteral("windows"), QStringLiteral("get"),
        QJsonObject{{QStringLiteral("id"), wid}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QVERIFY(!result(reply).contains(QLatin1String("tabs")));

    // getCurrent / getLastFocused honour the same getInfo.
    reply = backend.invoke(
        QStringLiteral("windows"), QStringLiteral("getCurrent"),
        QJsonObject{{QStringLiteral("getInfo"),
                     QJsonObject{{QStringLiteral("populate"),
                                  true}}}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QVERIFY(result(reply).value(QLatin1String("tabs"))
                .toArray().size() >= 1);
    reply = backend.invoke(
        QStringLiteral("windows"), QStringLiteral("getLastFocused"),
        QJsonObject{{QStringLiteral("getInfo"),
                     QJsonObject{{QStringLiteral("populate"),
                                  true}}}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QVERIFY(result(reply).value(QLatin1String("tabs"))
                .toArray().size() >= 1);
    // get(WINDOW_ID_CURRENT) resolves the asking window like
    // Chrome's special id does.
    reply = backend.invoke(
        QStringLiteral("windows"), QStringLiteral("get"),
        QJsonObject{{QStringLiteral("id"), -2}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QVERIFY(result(reply).value(QLatin1String("id")).toInt() > 0);

    // getAll populates every window it reports.
    reply = backend.invoke(
        QStringLiteral("windows"), QStringLiteral("getAll"),
        QJsonObject{{QStringLiteral("getInfo"),
                     QJsonObject{{QStringLiteral("populate"),
                                  true}}}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    const QJsonArray all =
        reply.value(QLatin1String("result")).toArray();
    QVERIFY(!all.isEmpty());
    for (const QJsonValue &w : all)
        QVERIFY(w.toObject().value(QLatin1String("tabs"))
                    .toArray().size() >= 1);

    // windowTypes honesty — "normal" is the only type Arktos has,
    // so a popup-only filter matches nothing.
    QCOMPARE(backend.invoke(
                 QStringLiteral("windows"), QStringLiteral("get"),
                 QJsonObject{{QStringLiteral("id"), wid},
                             {QStringLiteral("getInfo"),
                              QJsonObject{{QStringLiteral("windowTypes"),
                                           QJsonArray{QStringLiteral(
                                               "popup")}}}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("not-found"));
    reply = backend.invoke(
        QStringLiteral("windows"), QStringLiteral("getAll"),
        QJsonObject{{QStringLiteral("getInfo"),
                     QJsonObject{{QStringLiteral("windowTypes"),
                                  QJsonArray{QStringLiteral(
                                      "popup")}}}}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QCOMPARE(reply.value(QLatin1String("result")).toArray().size(),
             0);
    reply = backend.invoke(
        QStringLiteral("windows"), QStringLiteral("getAll"),
        QJsonObject{{QStringLiteral("getInfo"),
                     QJsonObject{{QStringLiteral("windowTypes"),
                                  QJsonArray{QStringLiteral("normal"),
                                             QStringLiteral(
                                                 "popup")}}}}});
    QVERIFY(reply.value(QLatin1String("result")).toArray().size()
            >= 1);

    // --- real state ---------------------------------------------------
    // The window-state vocabulary reports what Qt reports.
    window->showMaximized();
    reply = backend.invoke(
        QStringLiteral("windows"), QStringLiteral("get"),
        QJsonObject{{QStringLiteral("id"), wid}});
    QCOMPARE(result(reply).value(QLatin1String("state")).toString(),
             QStringLiteral("maximized"));
    window->showNormal();
    reply = backend.invoke(
        QStringLiteral("windows"), QStringLiteral("get"),
        QJsonObject{{QStringLiteral("id"), wid}});
    QCOMPARE(result(reply).value(QLatin1String("state")).toString(),
             QStringLiteral("normal"));

    // --- update: bounds + state + drawAttention ----------------------
    reply = backend.invoke(
        QStringLiteral("windows"), QStringLiteral("update"),
        QJsonObject{{QStringLiteral("id"), wid},
                    {QStringLiteral("props"),
                     QJsonObject{{QStringLiteral("left"), 61},
                                 {QStringLiteral("top"), 37},
                                 {QStringLiteral("width"), 733},
                                 {QStringLiteral("height"), 442},
                                 {QStringLiteral("drawAttention"),
                                  true}}}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    // The reply reports the widget's frame-inclusive bounds — and
    // the widget itself moved.
    const QRect bounds = window->frameGeometry();
    QCOMPARE(window->x(), 61);
    QCOMPARE(window->y(), 37);
    QCOMPARE(window->width(), 733);
    QCOMPARE(window->height(), 442);
    win = result(reply);
    QCOMPARE(win.value(QLatin1String("left")).toInt(), bounds.x());
    QCOMPARE(win.value(QLatin1String("top")).toInt(), bounds.y());
    QCOMPARE(win.value(QLatin1String("width")).toInt(),
             bounds.width());
    QCOMPARE(win.value(QLatin1String("height")).toInt(),
             bounds.height());

    // update state lands on the real widget, and the roundtrip
    // back to normal clears it.
    reply = backend.invoke(
        QStringLiteral("windows"), QStringLiteral("update"),
        QJsonObject{{QStringLiteral("id"), wid},
                    {QStringLiteral("props"),
                     QJsonObject{{QStringLiteral("state"),
                                  QStringLiteral("maximized")}}}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QVERIFY(window->isMaximized());
    QCOMPARE(result(reply).value(QLatin1String("state")).toString(),
             QStringLiteral("maximized"));
    reply = backend.invoke(
        QStringLiteral("windows"), QStringLiteral("update"),
        QJsonObject{{QStringLiteral("id"), wid},
                    {QStringLiteral("props"),
                     QJsonObject{{QStringLiteral("state"),
                                  QStringLiteral("normal")}}}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QVERIFY(!window->isMaximized() && !window->isMinimized());

    // --- create honours the same props -------------------------------
    reply = backend.invoke(
        QStringLiteral("windows"), QStringLiteral("create"),
        QJsonObject{{QStringLiteral("props"),
                     QJsonObject{{QStringLiteral("left"), 20},
                                 {QStringLiteral("top"), 25},
                                 {QStringLiteral("width"), 640},
                                 {QStringLiteral("height"), 480},
                                 {QStringLiteral("focused"),
                                  false}}}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    const QJsonObject created = result(reply);
    const int createdId =
        created.value(QLatin1String("id")).toInt();
    QVERIFY(createdId > 0);
    // Chrome populates the new window's tabs array.
    QVERIFY(created.value(QLatin1String("tabs")).toArray().size()
            >= 1);
    // The minted widget itself carries the requested bounds.
    BrowserMainWindow *spawned = nullptr;
    for (QWidget *top : QApplication::topLevelWidgets()) {
        BrowserMainWindow *cand =
            qobject_cast<BrowserMainWindow *>(top);
        if (cand
                && ExtensionShimBackend::windowIdFor(cand)
                       == createdId) {
            spawned = cand;
            break;
        }
    }
    QVERIFY(spawned);
    QCOMPARE(spawned->x(), 20);
    QCOMPARE(spawned->y(), 25);
    QCOMPARE(spawned->width(), 640);
    QCOMPARE(spawned->height(), 480);
    QCOMPARE(created.value(QLatin1String("left")).toInt(),
             spawned->frameGeometry().x());
    QCOMPARE(created.value(QLatin1String("width")).toInt(),
             spawned->frameGeometry().width());

    // deleteLater, not close(): open tabs trip the modal
    // confirmClosingMultipleTabs prompt, unanswerable headless.
    spawned->deleteLater();
    window->deleteLater();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QDir(dir).removeRecursively();
}

// MD14 — chrome.action/browserAction.setIcon: path details resolve
// to bundle-relative files under the extension dir with
// openOptionsPage's hygiene, imageData details carry raw RGBA (the
// shim base64s them across the bridge); both record into ActionState
// and surface through ExtensionInfo for the EXT08 strip.  The 'get'
// icon member is the read-back readers/tests use — Chrome exposes
// no getIcon.
void tst_ExtensionManager::extensionActionIcon()
{
    const QString dir = QDir::temp().filePath(
        QStringLiteral("tst-exticon-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dir));
    {
        QFile f(dir + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-icon\","
                "\"version\":\"1.0\"}");
        f.close();
    }
    const QString id = QStringLiteral("tstmockextensionid0007");
    ExtensionShimBackend backend(id, QStringLiteral("tst-icon"), dir);

    const auto result = [](const QJsonObject &reply) {
        return reply.value(QLatin1String("result")).toObject();
    };

    // --- imageData: raw RGBA decoded into the recorded state --------
    // 2x2 image — red, green, blue, opaque-white pixels.
    const QByteArray rgba = QByteArray::fromHex(
        "ff0000ff00ff00ff0000ffffffffffff");
    QJsonObject reply = backend.invoke(
        QStringLiteral("action"), QStringLiteral("setIcon"),
        QJsonObject{{QStringLiteral("imageData"),
            QJsonObject{{QStringLiteral("width"), 2},
                        {QStringLiteral("height"), 2},
                        {QStringLiteral("rgba"),
                         QString::fromLatin1(rgba.toBase64())}}}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    ExtensionShimBackend::ActionState state =
        ExtensionShimBackend::actionState(id);
    QCOMPARE(state.iconImages.size(), 1);
    const QImage img = state.iconImages.value(0);
    QCOMPARE(img.size(), QSize(2, 2));
    QCOMPARE(img.pixelColor(0, 0), QColor(255, 0, 0));
    QCOMPARE(img.pixelColor(1, 0), QColor(0, 255, 0));
    QCOMPARE(img.pixelColor(0, 1), QColor(0, 0, 255));
    QCOMPARE(img.pixelColor(1, 1), QColor(255, 255, 255));
    QVERIFY(state.iconPaths.isEmpty());
    // The 'get' reader sees it.
    reply = backend.invoke(
        QStringLiteral("action"), QStringLiteral("get"),
        QJsonObject{{QStringLiteral("key"),
                     QStringLiteral("icon")}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QCOMPARE(result(reply).value(QLatin1String("imageDataSizes"))
                 .toArray(),
             QJsonArray{0});

    // --- path map through the browserAction spelling ----------------
    // A later setIcon replaces the whole override — the recorded
    // imageData drops when only path is given (Chrome parity).
    reply = backend.invoke(
        QStringLiteral("browserAction"), QStringLiteral("setIcon"),
        QJsonObject{{QStringLiteral("path"),
            QJsonObject{{QStringLiteral("16"),
                         QStringLiteral("icons/a.png")},
                        {QStringLiteral("32"),
                         QStringLiteral("icons/b.png")}}}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    state = ExtensionShimBackend::actionState(id);
    QCOMPARE(state.iconPaths.size(), 2);
    QCOMPARE(state.iconPaths.value(16),
             QStringLiteral("icons/a.png"));
    QCOMPARE(state.iconPaths.value(32),
             QStringLiteral("icons/b.png"));
    QVERIFY(state.iconImages.isEmpty());
    reply = backend.invoke(
        QStringLiteral("action"), QStringLiteral("get"),
        QJsonObject{{QStringLiteral("key"),
                     QStringLiteral("icon")}});
    QCOMPARE(result(reply).value(QLatin1String("paths")).toObject()
                 .value(QLatin1String("16")).toString(),
             QStringLiteral("icons/a.png"));

    // A bare-string path records under the unsized key.
    reply = backend.invoke(
        QStringLiteral("action"), QStringLiteral("setIcon"),
        QJsonObject{{QStringLiteral("path"),
                     QStringLiteral("icon.png")}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QCOMPARE(ExtensionShimBackend::actionState(id)
                 .iconPaths.value(0),
             QStringLiteral("icon.png"));

    // --- hygiene: escapes and absolutes are typed refusals ----------
    for (const QString &bad : {QStringLiteral("../escape.png"),
                               QStringLiteral("/abs.png"),
                               QStringLiteral("a:b.png"),
                               QStringLiteral("icons/../x.png"),
                               QStringLiteral("https://e/x.png")}) {
        reply = backend.invoke(
            QStringLiteral("action"), QStringLiteral("setIcon"),
            QJsonObject{{QStringLiteral("path"), bad}});
        QCOMPARE(reply.value(QLatin1String("error")).toString(),
                 QStringLiteral("invalid-path"));
    }
    // Same through a size map — and a failed call must not
    // half-commit (the recorded override is untouched).
    reply = backend.invoke(
        QStringLiteral("action"), QStringLiteral("setIcon"),
        QJsonObject{{QStringLiteral("path"),
            QJsonObject{{QStringLiteral("16"),
                         QStringLiteral("icons/a.png")},
                        {QStringLiteral("32"),
                         QStringLiteral("../x.png")}}}});
    QCOMPARE(reply.value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-path"));
    QCOMPARE(ExtensionShimBackend::actionState(id)
                 .iconPaths.value(0),
             QStringLiteral("icon.png"));

    // Malformed imageData is its own typed refusal.
    reply = backend.invoke(
        QStringLiteral("action"), QStringLiteral("setIcon"),
        QJsonObject{{QStringLiteral("imageData"),
            QJsonObject{{QStringLiteral("width"), 4},
                        {QStringLiteral("height"), 4},
                        {QStringLiteral("rgba"),
                         QString::fromLatin1(rgba.toBase64())}}}});
    QCOMPARE(reply.value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-image-data"));

    // --- setIcon({}) resets to the manifest default -----------------
    reply = backend.invoke(
        QStringLiteral("action"), QStringLiteral("setIcon"),
        QJsonObject());
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    state = ExtensionShimBackend::actionState(id);
    QVERIFY(state.iconPaths.isEmpty() && state.iconImages.isEmpty());
    reply = backend.invoke(
        QStringLiteral("action"), QStringLiteral("get"),
        QJsonObject{{QStringLiteral("key"),
                     QStringLiteral("icon")}});
    QVERIFY(result(reply).value(QLatin1String("paths")).toObject()
                .isEmpty());
    QVERIFY(result(reply).value(QLatin1String("imageDataSizes"))
                .toArray().isEmpty());

    // --- ExtensionInfo carries the override for the strip -----------
    // Needs a real manager row, so this half installs the fixture.
    if (ExtensionManager::isSupported()) {
        const QString realId = installFixture(dir);
        QVERIFY2(!realId.isEmpty()
                     && !realId.startsWith(QLatin1String("ERROR:")),
                 qPrintable(realId));
        ExtensionShimBackend live(realId, QStringLiteral("tst-icon"),
                                  dir);
        reply = live.invoke(
            QStringLiteral("action"), QStringLiteral("setIcon"),
            QJsonObject{{QStringLiteral("imageData"),
                QJsonObject{{QStringLiteral("width"), 2},
                            {QStringLiteral("height"), 2},
                            {QStringLiteral("rgba"),
                             QString::fromLatin1(
                                 rgba.toBase64())}}}});
        QVERIFY(reply.value(QLatin1String("ok")).toBool());
        bool saw = false;
        for (const ExtensionManager::ExtensionInfo &info
             : ExtensionManager::instance()->extensions()) {
            if (info.id == realId) {
                saw = info.actionIconImages.value(0).size()
                        == QSize(2, 2)
                    && info.actionIconImages.value(0).pixelColor(1, 0)
                        == QColor(0, 255, 0);
            }
        }
        QVERIFY(saw);
        ExtensionManager::instance()->removeExtension(realId);
    }

    QDir(dir).removeRecursively();
}

// MD15 — chrome.extension.isAllowedIncognitoAccess /
// isAllowedFileSchemeAccess.  The verdicts route through
// invokeExtension: Arktos hosts every extension context on the
// regular profile (no incognito split) and the loader never grants
// file: scheme access, so both answer {ok:true,false} — the truth,
// not a typed stub.
void tst_ExtensionManager::extensionAllowedAccess()
{
    const QString dir = QDir::temp().filePath(
        QStringLiteral("tst-extaccess-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dir));
    {
        QFile f(dir + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-access\","
                "\"version\":\"1.0\"}");
        f.close();
    }
    ExtensionShimBackend backend(
        QStringLiteral("tstmockextensionid0008"),
        QStringLiteral("tst-access"), dir);

    for (const char *m : {"isAllowedIncognitoAccess",
                          "isAllowedFileSchemeAccess"}) {
        const QJsonObject reply = backend.invoke(
            QStringLiteral("extension"), QLatin1String(m),
            QJsonObject());
        QVERIFY2(reply.value(QLatin1String("ok")).toBool(), m);
        QVERIFY2(reply.value(QLatin1String("result")).isBool(), m);
        QVERIFY2(!reply.value(QLatin1String("result")).toBool(), m);
    }
    // Unserved extension members still degrade typed.
    QCOMPARE(backend.invoke(QStringLiteral("extension"),
                            QStringLiteral("bogus"),
                            QJsonObject())
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("unimplemented"));

    QDir(dir).removeRecursively();
}

// MD16 — chrome.management read ops.  getAll/get enumerate the
// manager's live ExtensionInfo rows through the same shape getSelf
// answers; the permissionWarnings pair derive their strings from the
// manifest's grant list — the same wording the install review shows.
// Everything rides the 'management' grant except getSelf.
void tst_ExtensionManager::extensionManagementRead()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");

    const QString dir = QDir::temp().filePath(
        QStringLiteral("tst-extmgmt-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dir));
    {
        QFile f(dir + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-mgmt\","
                "\"version\":\"3.1\",\"permissions\":[\"management\","
                "\"tabs\",\"history\"],\"host_permissions\":"
                "[\"https://*.example.com/*\"]}");
        f.close();
    }
    ExtensionManager *manager = ExtensionManager::instance();
    auto loaded = std::make_shared<QString>();
    QObject::connect(manager, &ExtensionManager::extensionLoaded,
                     this, [loaded](
                         const ExtensionManager::ExtensionInfo &info) {
        if (info.name == QLatin1String("tst-mgmt")
                && info.error.isEmpty())
            *loaded = info.id;
    });
    manager->grantConsent(dir);
    manager->loadExtension(dir);
    QTRY_VERIFY_WITH_TIMEOUT(!loaded->isEmpty(), 30000);
    const QString id = *loaded;

    ExtensionShimBackend backend(id, QStringLiteral("tst-mgmt"), dir);

    // getAll enumerates the manager's records — the fixture is there
    // with the same shape getSelf builds.
    QJsonObject reply = backend.invoke(
        QStringLiteral("management"), QStringLiteral("getAll"),
        QJsonObject());
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    const QJsonArray all =
        reply.value(QLatin1String("result")).toArray();
    QVERIFY(!all.isEmpty());
    bool found = false;
    for (const QJsonValue &v : all) {
        const QJsonObject info = v.toObject();
        if (info.value(QLatin1String("id")).toString() != id)
            continue;
        found = true;
        QCOMPARE(info.value(QLatin1String("name")).toString(),
                 QStringLiteral("tst-mgmt"));
        QCOMPARE(info.value(QLatin1String("version")).toString(),
                 QStringLiteral("3.1"));
        QCOMPARE(info.value(QLatin1String("type")).toString(),
                 QStringLiteral("extension"));
        // Freshly loaded, never enabled — the record is the truth.
        QVERIFY(!info.value(QLatin1String("enabled")).toBool());
        QVERIFY(info.value(QLatin1String("mayEnable")).toBool());
        QCOMPARE(info.value(QLatin1String("installType")).toString(),
                 QStringLiteral("development"));
        QCOMPARE(info.value(QLatin1String("manifestVersion")).toInt(),
                 3);
        QVERIFY(info.value(QLatin1String("permissions")).toArray()
                .contains(QJsonValue(QStringLiteral("tabs"))));
        QVERIFY(info.value(QLatin1String("permissions")).toArray()
                .contains(QJsonValue(QStringLiteral("management"))));
        QVERIFY(info.value(QLatin1String("hostPermissions")).toArray()
                .contains(QJsonValue(
                    QStringLiteral("https://*.example.com/*"))));
    }
    QVERIFY2(found, "fixture extension missing from getAll");

    // get answers the same shape for one id; unknown ids are the
    // typed refusal Chrome's lastError carries.
    reply = backend.invoke(
        QStringLiteral("management"), QStringLiteral("get"),
        QJsonObject{{QStringLiteral("id"), id}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QCOMPARE(reply.value(QLatin1String("result")).toObject()
                 .value(QLatin1String("id")).toString(), id);
    QCOMPARE(reply.value(QLatin1String("result")).toObject()
                 .value(QLatin1String("version")).toString(),
             QStringLiteral("3.1"));
    QCOMPARE(backend.invoke(
                 QStringLiteral("management"), QStringLiteral("get"),
                 QJsonObject{{QStringLiteral("id"),
                              QStringLiteral("nonexistentid000000000"
                                             "00000000000")}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("not-found"));
    QCOMPARE(backend.invoke(
                 QStringLiteral("management"), QStringLiteral("get"),
                 QJsonObject())
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));

    // getSelf keeps its ungated answer, consistent with the row
    // getAll enumerated.
    reply = backend.invoke(
        QStringLiteral("management"), QStringLiteral("getSelf"),
        QJsonObject());
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QCOMPARE(reply.value(QLatin1String("result")).toObject()
                 .value(QLatin1String("id")).toString(), id);
    QCOMPARE(reply.value(QLatin1String("result")).toObject()
                 .value(QLatin1String("manifestVersion")).toInt(), 3);

    // Permission warnings derive from the manifest's grant list —
    // the same strings the install review shows, deduplicated.
    reply = backend.invoke(
        QStringLiteral("management"),
        QStringLiteral("getPermissionWarningsById"),
        QJsonObject{{QStringLiteral("id"), id}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QStringList warnings;
    for (const QJsonValue &w :
         reply.value(QLatin1String("result")).toArray())
        warnings.append(w.toString());
    QVERIFY(warnings.contains(
        ExtensionReviewDialog::describePermission(
            QStringLiteral("history"))));
    QVERIFY(warnings.contains(
        ExtensionReviewDialog::describePermission(
            QStringLiteral("tabs"))));
    QVERIFY(warnings.contains(
        ExtensionReviewDialog::describeHostPermission(
            QStringLiteral("https://*.example.com/*"))));
    QCOMPARE(backend.invoke(
                 QStringLiteral("management"),
                 QStringLiteral("getPermissionWarningsById"),
                 QJsonObject{{QStringLiteral("id"),
                              QStringLiteral("nonexistentid000000000"
                                             "00000000000")}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("not-found"));

    // ByManifest takes Chrome's manifest-string argument (a parsed
    // object is accepted too); a malformed payload is typed.
    reply = backend.invoke(
        QStringLiteral("management"),
        QStringLiteral("getPermissionWarningsByManifest"),
        QJsonObject{{QStringLiteral("manifest"),
                     QStringLiteral(
                         "{\"permissions\":[\"bookmarks\"],"
                         "\"host_permissions\":[\"<all_urls>\"]}")}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    warnings.clear();
    for (const QJsonValue &w :
         reply.value(QLatin1String("result")).toArray())
        warnings.append(w.toString());
    QVERIFY(warnings.contains(
        ExtensionReviewDialog::describePermission(
            QStringLiteral("bookmarks"))));
    QVERIFY(warnings.contains(
        ExtensionReviewDialog::describeHostPermission(
            QStringLiteral("<all_urls>"))));
    QCOMPARE(backend.invoke(
                 QStringLiteral("management"),
                 QStringLiteral("getPermissionWarningsByManifest"),
                 QJsonObject{{QStringLiteral("manifest"),
                              QStringLiteral("{bogus")}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));

    // The 'management' grant gates every read op — an extension that
    // never declared it keeps Chrome's no-permission answer while
    // getSelf stays free.
    const QString dirB = QDir::temp().filePath(
        QStringLiteral("tst-extmgmt-b-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dirB));
    {
        QFile f(dirB + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-mgmt-b\","
                "\"version\":\"1.0\",\"permissions\":[\"storage\"]}");
        f.close();
    }
    ExtensionShimBackend backendB(
        QStringLiteral("tstmockextensionid0009"),
        QStringLiteral("tst-mgmt-b"), dirB);
    QCOMPARE(backendB.invoke(
                 QStringLiteral("management"),
                 QStringLiteral("getAll"), QJsonObject())
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("no-permission"));
    QCOMPARE(backendB.invoke(
                 QStringLiteral("management"), QStringLiteral("get"),
                 QJsonObject{{QStringLiteral("id"), id}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("no-permission"));
    QCOMPARE(backendB.invoke(
                 QStringLiteral("management"),
                 QStringLiteral("getPermissionWarningsById"),
                 QJsonObject{{QStringLiteral("id"), id}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("no-permission"));
    QVERIFY(backendB.invoke(
                QStringLiteral("management"),
                QStringLiteral("getSelf"), QJsonObject())
                .value(QLatin1String("ok")).toBool());
    // Declared-but-unserved members keep the typed answer — MD17
    // took the mutations, so the launch ops carry it now.
    QCOMPARE(backend.invoke(
                 QStringLiteral("management"),
                 QStringLiteral("launchApp"),
                 QJsonObject{{QStringLiteral("id"), id}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("unsupported"));

    manager->removeExtension(id);
    QDir(dir).removeRecursively();
    QDir(dirB).removeRecursively();
}

// MD17 — chrome.management mutations.  setEnabled/uninstall ride
// ExtensionManager's engine calls behind the 'management' grant;
// each committed transition fans its Chrome event (onEnabled/
// onDisabled carrying the post-change ExtensionInfo, onUninstalled
// carrying the bare id) out through the runtime broadcast — a bare
// backend hears it on its own pushEvent sink.
void tst_ExtensionManager::extensionManagementMutations()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");

    const QString dir = QDir::temp().filePath(
        QStringLiteral("tst-extmgmtmut-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dir));
    {
        QFile f(dir + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-mgmtmut\","
                "\"version\":\"1.0\",\"permissions\":[\"management\"]}");
        f.close();
    }
    ExtensionManager *manager = ExtensionManager::instance();
    auto loaded = std::make_shared<QString>();
    QObject::connect(manager, &ExtensionManager::extensionLoaded,
                     this, [loaded](
                         const ExtensionManager::ExtensionInfo &info) {
        if (info.name == QLatin1String("tst-mgmtmut")
                && info.error.isEmpty())
            *loaded = info.id;
    });
    manager->grantConsent(dir);
    manager->loadExtension(dir);
    QTRY_VERIFY_WITH_TIMEOUT(!loaded->isEmpty(), 30000);
    const QString id = *loaded;

    ExtensionShimBackend backend(id, QStringLiteral("tst-mgmtmut"), dir);
    QSignalSpy pushSpy(&backend, &ExtensionShimBackend::pushEvent);

    // Freshly dev-loaded records enumerate disabled — the first
    // toggle on is a real transition: management.onEnabled carries
    // the post-change ExtensionInfo shape get() answers.
    QJsonObject reply = backend.invoke(
        QStringLiteral("management"), QStringLiteral("setEnabled"),
        QJsonObject{{QStringLiteral("id"), id},
                    {QStringLiteral("enabled"), true}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QCOMPARE(pushCount(pushSpy, QStringLiteral("management.onEnabled")),
             1);
    for (const QList<QVariant> &args : pushSpy) {
        if (args.at(0).toString()
                != QLatin1String("management.onEnabled"))
            continue;
        const QJsonObject info = args.at(1).toJsonObject()
            .value(QLatin1String("__args")).toArray()
            .at(0).toObject();
        QCOMPARE(info.value(QLatin1String("id")).toString(), id);
        QVERIFY(info.value(QLatin1String("enabled")).toBool());
        QVERIFY(!info.value(QLatin1String("mayEnable")).toBool());
        QVERIFY(info.value(QLatin1String("mayDisable")).toBool());
    }
    // The engine applies asynchronously — poll the live record.
    QTRY_VERIFY_WITH_TIMEOUT(
        backend.invoke(QStringLiteral("management"),
                       QStringLiteral("get"),
                       QJsonObject{{QStringLiteral("id"), id}})
            .value(QLatin1String("result")).toObject()
            .value(QLatin1String("enabled")).toBool(), 10000);

    // Off again — management.onDisabled carries enabled:false.
    reply = backend.invoke(
        QStringLiteral("management"), QStringLiteral("setEnabled"),
        QJsonObject{{QStringLiteral("id"), id},
                    {QStringLiteral("enabled"), false}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QCOMPARE(pushCount(pushSpy, QStringLiteral("management.onDisabled")),
             1);
    QTRY_VERIFY_WITH_TIMEOUT(
        !backend.invoke(QStringLiteral("management"),
                        QStringLiteral("get"),
                        QJsonObject{{QStringLiteral("id"), id}})
             .value(QLatin1String("result")).toObject()
             .value(QLatin1String("enabled")).toBool(), 10000);

    // A same-state setEnabled is Chrome's no-op success — the
    // transition events describe real changes only.
    reply = backend.invoke(
        QStringLiteral("management"), QStringLiteral("setEnabled"),
        QJsonObject{{QStringLiteral("id"), id},
                    {QStringLiteral("enabled"), false}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QCOMPARE(pushCount(pushSpy, QStringLiteral("management.onDisabled")),
             1);

    // Typed refusals: missing id, unknown id.
    QCOMPARE(backend.invoke(
                 QStringLiteral("management"),
                 QStringLiteral("setEnabled"), QJsonObject())
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    QCOMPARE(backend.invoke(
                 QStringLiteral("management"),
                 QStringLiteral("setEnabled"),
                 QJsonObject{{QStringLiteral("id"),
                              QStringLiteral("nonexistentid000000000"
                                             "00000000000")},
                             {QStringLiteral("enabled"), true}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("not-found"));
    QCOMPARE(backend.invoke(
                 QStringLiteral("management"),
                 QStringLiteral("uninstall"), QJsonObject())
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    QCOMPARE(backend.invoke(
                 QStringLiteral("management"),
                 QStringLiteral("uninstall"),
                 QJsonObject{{QStringLiteral("id"),
                              QStringLiteral("nonexistentid000000000"
                                             "00000000000")}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("not-found"));

    // The 'management' grant gates all three mutations — an
    // extension that never declared it keeps no-permission.
    const QString dirB = QDir::temp().filePath(
        QStringLiteral("tst-extmgmtmut-b-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dirB));
    {
        QFile f(dirB + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-mgmtmut-b\","
                "\"version\":\"1.0\",\"permissions\":[\"storage\"]}");
        f.close();
    }
    ExtensionShimBackend backendB(
        QStringLiteral("tstmockextensionid0010"),
        QStringLiteral("tst-mgmtmut-b"), dirB);
    for (const char *m : {"setEnabled", "uninstall", "uninstallSelf"}) {
        QCOMPARE(backendB.invoke(
                     QStringLiteral("management"), QLatin1String(m),
                     QJsonObject{{QStringLiteral("id"), id},
                                 {QStringLiteral("enabled"), true}})
                     .value(QLatin1String("error")).toString(),
                 QStringLiteral("no-permission"));
    }

    // uninstall removes the fixture for real — onUninstalled fires
    // with the bare id like Chrome, and get stops finding it.
    reply = backend.invoke(
        QStringLiteral("management"), QStringLiteral("uninstall"),
        QJsonObject{{QStringLiteral("id"), id}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QCOMPARE(pushCount(pushSpy,
                       QStringLiteral("management.onUninstalled")), 1);
    for (const QList<QVariant> &args : pushSpy) {
        if (args.at(0).toString()
                != QLatin1String("management.onUninstalled"))
            continue;
        QCOMPARE(args.at(1).toJsonObject()
                     .value(QLatin1String("__args")).toArray()
                     .at(0).toString(), id);
    }
    QTRY_COMPARE_WITH_TIMEOUT(
        backend.invoke(QStringLiteral("management"),
                       QStringLiteral("get"),
                       QJsonObject{{QStringLiteral("id"), id}})
            .value(QLatin1String("error")).toString(),
        QStringLiteral("not-found"), 10000);

    // uninstallSelf drops the calling extension itself — same
    // 'management' gate, same onUninstalled id payload.
    const QString dirC = QDir::temp().filePath(
        QStringLiteral("tst-extmgmtmut-c-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dirC));
    {
        QFile f(dirC + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-mgmtmut-c\","
                "\"version\":\"1.0\",\"permissions\":[\"management\"]}");
        f.close();
    }
    auto loadedC = std::make_shared<QString>();
    QObject::connect(manager, &ExtensionManager::extensionLoaded,
                     this, [loadedC](
                         const ExtensionManager::ExtensionInfo &info) {
        if (info.name == QLatin1String("tst-mgmtmut-c")
                && info.error.isEmpty())
            *loadedC = info.id;
    });
    manager->grantConsent(dirC);
    manager->loadExtension(dirC);
    QTRY_VERIFY_WITH_TIMEOUT(!loadedC->isEmpty(), 30000);
    const QString idC = *loadedC;
    ExtensionShimBackend backendC(idC, QStringLiteral("tst-mgmtmut-c"),
                                  dirC);
    QSignalSpy pushSpyC(&backendC, &ExtensionShimBackend::pushEvent);
    reply = backendC.invoke(
        QStringLiteral("management"), QStringLiteral("uninstallSelf"),
        QJsonObject());
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QCOMPARE(pushCount(pushSpyC,
                       QStringLiteral("management.onUninstalled")), 1);
    QTRY_COMPARE_WITH_TIMEOUT(
        backendC.invoke(QStringLiteral("management"),
                        QStringLiteral("get"),
                        QJsonObject{{QStringLiteral("id"), idC}})
            .value(QLatin1String("error")).toString(),
        QStringLiteral("not-found"), 10000);

    QDir(dir).removeRecursively();
    QDir(dirB).removeRecursively();
    QDir(dirC).removeRecursively();
}

// MD21 — chrome.bookmarks served over the RC05 store.  A dedicated
// temp persona is bound as the ambient manager — the backend's
// BookmarksManager::instance() then resolves exactly this tree —
// and the leg drives the full member surface: the fixed "0"/"1"/"2"
// ids, create/move/update/remove/removeTree through the manager's
// undoable ops (the C++ tree stays in lockstep), Chrome's error
// vocabulary for refused ops, the 'bookmarks' permission gate, and
// the on* fan-out a bare backend hears on pushEvent.
void tst_ExtensionManager::extensionBookmarks()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");

    // A dedicated temp persona becomes the ambient profile — the
    // backend's BookmarksManager::instance() resolves exactly this
    // tree.  Binding it flushes the previous ambient manager's file
    // and marks that view stale (the store's designed single-binding
    // hand-off); the ambient id restores on exit and the persona
    // dir goes with it.
    const QString prevId = BrowserPaths::activeProfileId();
    const QString profileId = QStringLiteral("tstextbm%1")
            .arg(QCoreApplication::applicationPid());
    BrowserPaths::setActiveProfileId(profileId);
    BookmarksManager *manager = BookmarksManager::instance();
    QVERIFY(manager);
    QCOMPARE(manager->profileId(), profileId);
    auto unbind = qScopeGuard([prevId, profileId]() {
        BookmarksManager::releaseFor(profileId);
        BrowserPaths::setActiveProfileId(prevId);
        QDir(BrowserPaths::profileDataDir(profileId))
            .removeRecursively();
    });

    // A fresh persona loads the bundled defaults into the menu —
    // strip both fixed folders so the assertions name only what this
    // leg created.
    for (BookmarkNode *top :
         {manager->toolbar(), manager->menu()}) {
        while (!top->children().isEmpty())
            manager->removeBookmark(top->children().first());
    }
    const QString dir = QDir::temp().filePath(
        QStringLiteral("tst-extbm-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dir));
    {
        QFile f(dir + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-bm\","
                "\"version\":\"1.0\",\"permissions\":[\"bookmarks\"]}");
        f.close();
    }
    ExtensionShimBackend backend(
        QStringLiteral("tstmockextensionid0021"),
        QStringLiteral("tst-bm"), dir);
    QSignalSpy events(&backend, &ExtensionShimBackend::pushEvent);
    auto invoke = [&](const QString &m, const QJsonObject &args) {
        return backend.invoke(QStringLiteral("bookmarks"), m, args);
    };

    // getTree — root "0" carries the fixed "1"/"2" folders; folders
    // carry no url and no dates are fabricated for a store that does
    // not keep them.
    QJsonObject reply = invoke(QStringLiteral("getTree"),
                               QJsonObject());
    QVERIFY2(reply.value(QLatin1String("ok")).toBool(),
             qPrintable(reply.value(QLatin1String("error")).toString()));
    const QJsonArray tree = reply.value(QLatin1String("result")).toArray();
    QCOMPARE(tree.size(), 1);
    const QJsonObject root = tree.at(0).toObject();
    QCOMPARE(root.value(QLatin1String("id")).toString(),
             QStringLiteral("0"));
    QCOMPARE(root.value(QLatin1String("type")).toString(),
             QStringLiteral("folder"));
    QVERIFY(!root.contains(QLatin1String("url")));
    QVERIFY(!root.contains(QLatin1String("dateAdded")));
    const QJsonArray tops = root.value(QLatin1String("children")).toArray();
    QCOMPARE(tops.size(), 2);
    QCOMPARE(tops.at(0).toObject().value(QLatin1String("id")).toString(),
             QStringLiteral("1"));
    QCOMPARE(tops.at(1).toObject().value(QLatin1String("id")).toString(),
             QStringLiteral("2"));

    // create — a folder under "2", then a bookmark and a second
    // folder inside it; every op lands through the undoable manager
    // commands so the C++ tree reflects each one.
    reply = invoke(QStringLiteral("create"), QJsonObject{
        {QStringLiteral("props"), QJsonObject{
            {QStringLiteral("parentId"), QStringLiteral("2")},
            {QStringLiteral("title"), QStringLiteral("Faves")}}}});
    QVERIFY2(reply.value(QLatin1String("ok")).toBool(),
             qPrintable(reply.value(QLatin1String("error")).toString()));
    const QJsonObject folder =
            reply.value(QLatin1String("result")).toObject();
    const QString folderId = folder.value(QLatin1String("id")).toString();
    QVERIFY(folderId != QLatin1String("0")
            && folderId != QLatin1String("1")
            && folderId != QLatin1String("2"));
    QCOMPARE(folder.value(QLatin1String("type")).toString(),
             QStringLiteral("folder"));
    QCOMPARE(folder.value(QLatin1String("parentId")).toString(),
             QStringLiteral("2"));
    QCOMPARE(folder.value(QLatin1String("index")).toInt(), 0);
    QCOMPARE(pushCount(events, QStringLiteral("bookmarks.onCreated")),
             1);
    QCOMPARE(manager->menu()->children().size(), 1);
    QCOMPARE(manager->menu()->children().first()->title,
             QStringLiteral("Faves"));

    reply = invoke(QStringLiteral("create"), QJsonObject{
        {QStringLiteral("props"), QJsonObject{
            {QStringLiteral("parentId"), folderId},
            {QStringLiteral("title"), QStringLiteral("Arktos")},
            {QStringLiteral("url"),
             QStringLiteral("https://arktos-browser.com")}}}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    const QString bmId = reply.value(QLatin1String("result"))
            .toObject().value(QLatin1String("id")).toString();
    QVERIFY(!bmId.isEmpty());
    QCOMPARE(pushCount(events, QStringLiteral("bookmarks.onCreated")),
             2);

    reply = invoke(QStringLiteral("create"), QJsonObject{
        {QStringLiteral("props"), QJsonObject{
            {QStringLiteral("parentId"), folderId},
            {QStringLiteral("index"), 0},
            {QStringLiteral("title"), QStringLiteral("Sub")}}}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    const QString subId = reply.value(QLatin1String("result"))
            .toObject().value(QLatin1String("id")).toString();
    QCOMPARE(pushCount(events, QStringLiteral("bookmarks.onCreated")),
             3);

    // get — single and array forms share one reply shape; unknown
    // ids answer not-found, malformed arg shapes invalid-argument.
    reply = invoke(QStringLiteral("get"),
                   QJsonObject{{QStringLiteral("id"), bmId}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    const QJsonArray got = reply.value(QLatin1String("result")).toArray();
    QCOMPARE(got.size(), 1);
    QCOMPARE(got.at(0).toObject().value(QLatin1String("title")).toString(),
             QStringLiteral("Arktos"));
    QCOMPARE(got.at(0).toObject().value(QLatin1String("parentId"))
                 .toString(), folderId);
    QCOMPARE(got.at(0).toObject().value(QLatin1String("index")).toInt(),
             1);
    reply = invoke(QStringLiteral("get"), QJsonObject{
        {QStringLiteral("ids"), QJsonArray{
            QStringLiteral("1"), folderId}}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QCOMPARE(reply.value(QLatin1String("result")).toArray().size(), 2);
    QCOMPARE(invoke(QStringLiteral("get"), QJsonObject{
                 {QStringLiteral("id"), QStringLiteral("999999")}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("not-found"));
    QCOMPARE(invoke(QStringLiteral("get"), QJsonObject())
             .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));

    // getChildren — insertion order is the sibling order; leaf nodes
    // answer an empty list rather than not-found.
    reply = invoke(QStringLiteral("getChildren"),
                   QJsonObject{{QStringLiteral("id"), folderId}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    const QJsonArray kids =
            reply.value(QLatin1String("result")).toArray();
    QCOMPARE(kids.size(), 2);
    QCOMPARE(kids.at(0).toObject().value(QLatin1String("title")).toString(),
             QStringLiteral("Sub"));
    QCOMPARE(kids.at(1).toObject().value(QLatin1String("title")).toString(),
             QStringLiteral("Arktos"));
    reply = invoke(QStringLiteral("getChildren"),
                   QJsonObject{{QStringLiteral("id"), bmId}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QVERIFY(reply.value(QLatin1String("result")).toArray().isEmpty());
    QCOMPARE(invoke(QStringLiteral("getChildren"), QJsonObject{
                 {QStringLiteral("id"), QStringLiteral("bogus")}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("not-found"));

    // getSubTree — children serialize recursively.
    reply = invoke(QStringLiteral("getSubTree"),
                   QJsonObject{{QStringLiteral("id"), folderId}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    const QJsonObject subTree = reply.value(QLatin1String("result"))
            .toArray().at(0).toObject();
    QCOMPARE(subTree.value(QLatin1String("id")).toString(), folderId);
    QCOMPARE(subTree.value(QLatin1String("children"))
                 .toArray().size(), 2);

    // update — title/url writes land in the store and report through
    // onChanged's changeInfo; roots refuse, folder urls refuse, and
    // the scheme gate still runs through the store's insert policy.
    reply = invoke(QStringLiteral("update"), QJsonObject{
        {QStringLiteral("id"), bmId},
        {QStringLiteral("changes"), QJsonObject{
            {QStringLiteral("title"),
             QStringLiteral("Arktos browser")}}}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QCOMPARE(pushCount(events, QStringLiteral("bookmarks.onChanged")),
             1);
    reply = invoke(QStringLiteral("get"),
                   QJsonObject{{QStringLiteral("id"), bmId}});
    QCOMPARE(reply.value(QLatin1String("result")).toArray()
             .at(0).toObject().value(QLatin1String("title")).toString(),
             QStringLiteral("Arktos browser"));
    QCOMPARE(invoke(QStringLiteral("update"), QJsonObject{
                 {QStringLiteral("id"), QStringLiteral("1")},
                 {QStringLiteral("changes"), QJsonObject{
                     {QStringLiteral("title"), QStringLiteral("x")}}}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("cannot-modify"));
    QCOMPARE(invoke(QStringLiteral("update"), QJsonObject{
                 {QStringLiteral("id"), folderId},
                 {QStringLiteral("changes"), QJsonObject{
                     {QStringLiteral("url"), QStringLiteral("http://x")}}}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    QCOMPARE(invoke(QStringLiteral("update"), QJsonObject{
                 {QStringLiteral("id"), bmId},
                 {QStringLiteral("changes"), QJsonObject{
                     {QStringLiteral("url"),
                      QStringLiteral("javascript:alert(1)")}}}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("scheme-refused"));

    // search — string terms AND across title+url, quoted terms keep
    // their spaces, the object form narrows by verbatim title /
    // trailing-slash-tolerant url / containing-folder title.
    reply = invoke(QStringLiteral("search"), QJsonObject{
        {QStringLiteral("query"), QStringLiteral("arktos")}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QCOMPARE(reply.value(QLatin1String("result")).toArray().size(), 1);
    QCOMPARE(reply.value(QLatin1String("result")).toArray()
             .at(0).toObject().value(QLatin1String("id")).toString(),
             bmId);
    reply = invoke(QStringLiteral("search"), QJsonObject{
        {QStringLiteral("query"), QJsonObject{
            {QStringLiteral("url"),
             QStringLiteral("https://arktos-browser.com/")}}}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QCOMPARE(reply.value(QLatin1String("result")).toArray().size(), 1);
    reply = invoke(QStringLiteral("search"), QJsonObject{
        {QStringLiteral("query"), QJsonObject{
            {QStringLiteral("query"), QStringLiteral("arktos")},
            {QStringLiteral("folder"), QStringLiteral("Faves")}}}});
    QCOMPARE(reply.value(QLatin1String("result")).toArray().size(), 1);
    reply = invoke(QStringLiteral("search"), QJsonObject{
        {QStringLiteral("query"), QStringLiteral("nomatchxyz")}});
    QVERIFY(reply.value(QLatin1String("result")).toArray().isEmpty());

    // getRecent — the newest store serials first, bookmarks only.
    reply = invoke(QStringLiteral("getRecent"),
                   QJsonObject{{QStringLiteral("count"), 5}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    const QJsonArray recent =
            reply.value(QLatin1String("result")).toArray();
    QCOMPARE(recent.size(), 1);
    QCOMPARE(recent.at(0).toObject().value(QLatin1String("id")).toString(),
             bmId);

    // move — the refused shapes first, then a real reparent that
    // keeps the node's id (the store handle never changes) and
    // reports old/new placement through onMoved.
    QCOMPARE(invoke(QStringLiteral("move"), QJsonObject{
                 {QStringLiteral("id"), folderId},
                 {QStringLiteral("destination"), QJsonObject{
                     {QStringLiteral("parentId"), subId}}}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    QCOMPARE(invoke(QStringLiteral("move"), QJsonObject{
                 {QStringLiteral("id"), bmId},
                 {QStringLiteral("destination"), QJsonObject{
                     {QStringLiteral("parentId"), bmId}}}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-parent"));
    QCOMPARE(invoke(QStringLiteral("move"), QJsonObject{
                 {QStringLiteral("id"), QStringLiteral("0")},
                 {QStringLiteral("destination"), QJsonObject{
                     {QStringLiteral("index"), 0}}}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("cannot-modify"));
    reply = invoke(QStringLiteral("move"), QJsonObject{
        {QStringLiteral("id"), folderId},
        {QStringLiteral("destination"), QJsonObject{
            {QStringLiteral("parentId"), QStringLiteral("1")},
            {QStringLiteral("index"), 0}}}});
    QVERIFY2(reply.value(QLatin1String("ok")).toBool(),
             qPrintable(reply.value(QLatin1String("error")).toString()));
    QCOMPARE(reply.value(QLatin1String("result")).toObject()
             .value(QLatin1String("parentId")).toString(),
             QStringLiteral("1"));
    QCOMPARE(pushCount(events, QStringLiteral("bookmarks.onMoved")), 1);
    QCOMPARE(manager->toolbar()->children().size(), 1);
    QCOMPARE(manager->toolbar()->children().first()->title,
             QStringLiteral("Faves"));
    QJsonObject moveInfo;
    for (const QVariantList &entry : events) {
        if (entry.at(0).toString()
                == QLatin1String("bookmarks.onMoved"))
            moveInfo = entry.at(1).toJsonObject()
                    .value(QLatin1String("__args")).toArray()
                    .at(1).toObject();
    }
    QCOMPARE(moveInfo.value(QLatin1String("oldParentId")).toString(),
             QStringLiteral("2"));
    QCOMPARE(moveInfo.value(QLatin1String("parentId")).toString(),
             QStringLiteral("1"));

    // remove — leaf only, the non-empty folder refuses until
    // removeTree takes the whole subtree.
    QCOMPARE(invoke(QStringLiteral("remove"), QJsonObject{
                 {QStringLiteral("id"), folderId}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("folder-not-empty"));
    reply = invoke(QStringLiteral("remove"),
                   QJsonObject{{QStringLiteral("id"), bmId}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QCOMPARE(pushCount(events, QStringLiteral("bookmarks.onRemoved")),
             1);
    QCOMPARE(invoke(QStringLiteral("get"), QJsonObject{
                 {QStringLiteral("id"), bmId}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("not-found"));
    QCOMPARE(invoke(QStringLiteral("remove"), QJsonObject{
                 {QStringLiteral("id"), QStringLiteral("1")}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("cannot-modify"));

    reply = invoke(QStringLiteral("removeTree"),
                   QJsonObject{{QStringLiteral("id"), folderId}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QCOMPARE(pushCount(events, QStringLiteral("bookmarks.onRemoved")),
             2);
    QCOMPARE(invoke(QStringLiteral("get"), QJsonObject{
                 {QStringLiteral("id"), folderId}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("not-found"));
    QVERIFY(manager->toolbar()->children().isEmpty());

    // The manifest gate is per-context: a sibling backend without
    // 'bookmarks' hears no-permission on reads and writes alike.
    const QString dirB = QDir::temp().filePath(
        QStringLiteral("tst-extbmb-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dirB));
    {
        QFile f(dirB + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-bm-b\","
                "\"version\":\"1.0\",\"permissions\":[\"storage\"]}");
        f.close();
    }
    ExtensionShimBackend backendB(
        QStringLiteral("tstmockextensionid0022"),
        QStringLiteral("tst-bm-b"), dirB);
    QCOMPARE(backendB.invoke(QStringLiteral("bookmarks"),
                 QStringLiteral("get"),
                 QJsonObject{{QStringLiteral("id"), QStringLiteral("0")}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("no-permission"));
    QCOMPARE(backendB.invoke(QStringLiteral("bookmarks"),
                 QStringLiteral("create"),
                 QJsonObject{{QStringLiteral("props"), QJsonObject{
                     {QStringLiteral("title"), QStringLiteral("x")}}}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("no-permission"));

    QDir(dir).removeRecursively();
    QDir(dirB).removeRecursively();
}

// MD22 — chrome.history served over the RC04 store.  A dedicated
// temp persona is bound as the ambient profile — the backend's
// HistoryManager::instance() then resolves exactly this store — and
// the leg drives the member surface: addUrl's 'link' visit and the
// aggregate onVisited, search's term/range/maxResults shape,
// getVisits' VisitItem rows, the three delete ops' onVisitRemoved
// payloads, the scheme gate, and the 'history' permission gate.
void tst_ExtensionManager::extensionHistory()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");

    // Same ambient-persona binding the MD21 leg uses: the backend's
    // HistoryManager::instance() resolves exactly this store, the
    // ambient id restores on exit, the persona dir goes with it.
    const QString prevId = BrowserPaths::activeProfileId();
    const QString profileId = QStringLiteral("tstexthist%1")
            .arg(QCoreApplication::applicationPid());
    BrowserPaths::setActiveProfileId(profileId);
    HistoryManager *manager = HistoryManager::instance();
    QVERIFY(manager);
    QCOMPARE(manager->profileId(), profileId);
    auto unbind = qScopeGuard([prevId, profileId]() {
        HistoryManager::releaseFor(profileId);
        BrowserPaths::setActiveProfileId(prevId);
        QDir(BrowserPaths::profileDataDir(profileId))
            .removeRecursively();
    });
    manager->clear();
    QVERIFY(manager->history().isEmpty());

    const QString dir = QDir::temp().filePath(
        QStringLiteral("tst-exthist-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dir));
    {
        QFile f(dir + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-hist\","
                "\"version\":\"1.0\",\"permissions\":[\"history\"]}");
        f.close();
    }
    ExtensionShimBackend backend(
        QStringLiteral("tstmockextensionid0023"),
        QStringLiteral("tst-hist"), dir);
    QSignalSpy events(&backend, &ExtensionShimBackend::pushEvent);
    auto invoke = [&](const QString &m, const QJsonObject &args) {
        return backend.invoke(QStringLiteral("history"), m, args);
    };

    // addUrl — a 'link' visit at now; onVisited carries the url's
    // aggregated HistoryItem once.
    const QString url =
        QStringLiteral("https://arktos-history.example/path");
    QJsonObject reply = invoke(QStringLiteral("addUrl"), QJsonObject{
        {QStringLiteral("url"), url}});
    QVERIFY2(reply.value(QLatin1String("ok")).toBool(),
             qPrintable(reply.value(QLatin1String("error")).toString()));
    QCOMPARE(manager->history().size(), 1);
    QCOMPARE(pushCount(events, QStringLiteral("history.onVisited")), 1);

    // Refused shapes — missing/malformed urls, script schemes.
    QCOMPARE(invoke(QStringLiteral("addUrl"), QJsonObject())
             .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    QCOMPARE(invoke(QStringLiteral("addUrl"), QJsonObject{
                 {QStringLiteral("url"),
                  QStringLiteral("javascript:alert(1)")}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("scheme-refused"));
    QCOMPARE(manager->history().size(), 1);

    // A second visit of the same url aggregates into one item.
    reply = invoke(QStringLiteral("addUrl"), QJsonObject{
        {QStringLiteral("url"), url}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QCOMPARE(manager->history().size(), 2);
    QCOMPARE(pushCount(events, QStringLiteral("history.onVisited")), 2);

    // search — term AND-match on url/title, Chrome's per-url
    // aggregate shape, the 24h default window, maxResults bounds.
    reply = invoke(QStringLiteral("search"), QJsonObject{
        {QStringLiteral("query"), QJsonObject{
            {QStringLiteral("text"),
             QStringLiteral("arktos-history path")}}}});
    QVERIFY2(reply.value(QLatin1String("ok")).toBool(),
             qPrintable(reply.value(QLatin1String("error")).toString()));
    const QJsonArray hits =
        reply.value(QLatin1String("result")).toArray();
    QCOMPARE(hits.size(), 1);
    const QJsonObject item = hits.at(0).toObject();
    QCOMPARE(item.value(QLatin1String("url")).toString(), url);
    QVERIFY(item.value(QLatin1String("lastVisitTime")).toDouble() > 0);
    QCOMPARE(item.value(QLatin1String("visitCount")).toInt(), 2);
    QCOMPARE(item.value(QLatin1String("typedCount")).toInt(), 0);
    QVERIFY(!item.value(QLatin1String("id")).toString().isEmpty());
    // AND-match: one missing term drops the item; a missing query
    // object and a sub-1 maxResults answer typed refusals.
    reply = invoke(QStringLiteral("search"), QJsonObject{
        {QStringLiteral("query"), QJsonObject{
            {QStringLiteral("text"),
             QStringLiteral("arktos-history nomatch")}}}});
    QVERIFY(reply.value(QLatin1String("result")).toArray().isEmpty());
    QCOMPARE(invoke(QStringLiteral("search"), QJsonObject())
             .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    QCOMPARE(invoke(QStringLiteral("search"), QJsonObject{
                 {QStringLiteral("query"), QJsonObject{
                     {QStringLiteral("maxResults"), 0}}}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));

    // A 3-day-old visit seeded through the import seam falls outside
    // Chrome's default 24h window but inside an explicit startTime.
    const QString oldUrl = QStringLiteral("https://old.example/x");
    const qint64 oldMs = QDateTime::currentMSecsSinceEpoch()
        - 3 * 86400000;
    QList<HistoryEntry> seed = manager->history();
    seed.append(HistoryEntry(oldUrl,
        QDateTime::fromMSecsSinceEpoch(oldMs),
        QStringLiteral("Old page")));
    manager->setHistory(seed);
    QCOMPARE(manager->history().size(), 3);
    reply = invoke(QStringLiteral("search"), QJsonObject{
        {QStringLiteral("query"), QJsonObject{
            {QStringLiteral("text"), QStringLiteral("")}}}});
    QCOMPARE(reply.value(QLatin1String("result")).toArray().size(), 1);
    reply = invoke(QStringLiteral("search"), QJsonObject{
        {QStringLiteral("query"), QJsonObject{
            {QStringLiteral("text"), QStringLiteral("")},
            {QStringLiteral("startTime"),
             double(oldMs - 3600000)}}}});
    const QJsonArray wide =
        reply.value(QLatin1String("result")).toArray();
    QCOMPARE(wide.size(), 2);
    // Newest lastVisitTime first — the fresh url leads the old one.
    QCOMPARE(wide.at(0).toObject().value(QLatin1String("url"))
                 .toString(), url);
    QCOMPARE(wide.at(1).toObject().value(QLatin1String("url"))
                 .toString(), oldUrl);
    QCOMPARE(wide.at(1).toObject().value(QLatin1String("title"))
                 .toString(), QStringLiteral("Old page"));

    // getVisits — reverse-chronological VisitItems for one url;
    // transition honestly 'link' (untracked), unknown urls empty.
    reply = invoke(QStringLiteral("getVisits"),
                   QJsonObject{{QStringLiteral("url"), url}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    const QJsonArray visits =
        reply.value(QLatin1String("result")).toArray();
    QCOMPARE(visits.size(), 2);
    const QJsonObject visit = visits.at(0).toObject();
    QVERIFY(visit.value(QLatin1String("visitTime")).toDouble()
            >= visits.at(1).toObject()
                   .value(QLatin1String("visitTime")).toDouble());
    QCOMPARE(visit.value(QLatin1String("transition")).toString(),
             QStringLiteral("link"));
    QCOMPARE(visit.value(QLatin1String("referringVisitId")).toString(),
             QStringLiteral("0"));
    QVERIFY(visit.value(QLatin1String("visitId")).toDouble() > 0);
    QCOMPARE(visit.value(QLatin1String("id")).toString(),
             item.value(QLatin1String("id")).toString());
    QVERIFY(invoke(QStringLiteral("getVisits"), QJsonObject{
                 {QStringLiteral("url"),
                  QStringLiteral("https://never.example")}})
            .value(QLatin1String("result")).toArray().isEmpty());
    QCOMPARE(invoke(QStringLiteral("getVisits"), QJsonObject())
             .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));

    // deleteRange — bounds are required and inclusive; the removed
    // urls travel in one aggregate onVisitRemoved.
    QCOMPARE(invoke(QStringLiteral("deleteRange"), QJsonObject{
                 {QStringLiteral("startTime"), 0.0}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    reply = invoke(QStringLiteral("deleteRange"), QJsonObject{
        {QStringLiteral("startTime"), double(oldMs - 1000)},
        {QStringLiteral("endTime"), double(oldMs + 1000)}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QCOMPARE(manager->history().size(), 2);
    QCOMPARE(pushCount(events,
                       QStringLiteral("history.onVisitRemoved")), 1);
    {
        const QVariantList last = events.last();
        const QJsonObject removed = last.at(1).toJsonObject()
            .value(QLatin1String("__args")).toArray()
            .at(0).toObject();
        QCOMPARE(removed.value(QLatin1String("allHistory")).toBool(),
                 false);
        const QJsonArray urls =
            removed.value(QLatin1String("urls")).toArray();
        QCOMPARE(urls.size(), 1);
        QCOMPARE(urls.at(0).toString(), oldUrl);
    }

    // deleteUrl — every visit of the url goes in one call.
    reply = invoke(QStringLiteral("deleteUrl"),
                   QJsonObject{{QStringLiteral("url"), url}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QVERIFY(manager->history().isEmpty());
    QCOMPARE(pushCount(events,
                       QStringLiteral("history.onVisitRemoved")), 2);
    QCOMPARE(invoke(QStringLiteral("deleteUrl"), QJsonObject())
             .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));

    // deleteAll — the store's clear path, allHistory:true.
    invoke(QStringLiteral("addUrl"),
           QJsonObject{{QStringLiteral("url"), url}});
    reply = invoke(QStringLiteral("deleteAll"), QJsonObject());
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QVERIFY(manager->history().isEmpty());
    QCOMPARE(pushCount(events,
                       QStringLiteral("history.onVisitRemoved")), 3);
    {
        const QVariantList last = events.last();
        const QJsonObject removed = last.at(1).toJsonObject()
            .value(QLatin1String("__args")).toArray()
            .at(0).toObject();
        QCOMPARE(removed.value(QLatin1String("allHistory")).toBool(),
                 true);
        QVERIFY(removed.value(QLatin1String("urls")).toArray()
                .isEmpty());
    }

    // The manifest gate is per-context: a sibling backend without
    // 'history' hears no-permission on reads and writes alike.
    const QString dirB = QDir::temp().filePath(
        QStringLiteral("tst-exthistb-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dirB));
    {
        QFile f(dirB + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-hist-b\","
                "\"version\":\"1.0\",\"permissions\":[\"storage\"]}");
        f.close();
    }
    ExtensionShimBackend backendB(
        QStringLiteral("tstmockextensionid0024"),
        QStringLiteral("tst-hist-b"), dirB);
    QCOMPARE(backendB.invoke(QStringLiteral("history"),
                 QStringLiteral("search"), QJsonObject{
                     {QStringLiteral("query"), QJsonObject{
                         {QStringLiteral("text"),
                          QStringLiteral("")}}}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("no-permission"));
    QCOMPARE(backendB.invoke(QStringLiteral("history"),
                 QStringLiteral("deleteAll"), QJsonObject())
             .value(QLatin1String("error")).toString(),
             QStringLiteral("no-permission"));

    QDir(dir).removeRecursively();
    QDir(dirB).removeRecursively();
}

// MD23 — chrome.cookies over the RC15 persistent mirror plus the
// live QWebEngineCookieStore.  A dedicated temp persona is bound as
// the ambient profile — invokeCookies resolves exactly this jar —
// and a REAL hosted extension (cookies grant + host_permissions on
// *.example.test) drives bridge->invoke so the coverage gate runs
// against the runtime's registered grants.  Covers set/get on a
// covered host (the task's verify hint), the mirror read-back,
// the engine store's landed write, onChanged fan-out (emit +
// per-domain withhold), remove, stores listing, arg refusals and
// the 'cookies' manifest gate.
void tst_ExtensionManager::extensionCookies()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");

    // Fixture: cookies grant + host coverage of *.example.test.
    // Loaded BEFORE the ambient persona switch — ExtensionManager's
    // engine handle binds the profile(s) prepared for the ambient
    // persona, and the runtime's permission/coverage maps fill on
    // load regardless of which profile hosts the worker.
    const QString dir = QDir::temp().filePath(
        QStringLiteral("tst-extck-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dir));
    {
        QFile f(dir + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-cookies\","
                "\"version\":\"1.0\",\"permissions\":[\"cookies\"],"
                "\"host_permissions\":[\"https://*.example.test/*\"],"
                "\"background\":{\"service_worker\":\"sw.js\"}}");
        f.close();
        QFile w(dir + QLatin1String("/sw.js"));
        QVERIFY(w.open(QIODevice::WriteOnly));
        w.write("self.__ck=[];\n"
                "try{chrome.cookies.onChanged.addListener("
                "function(ci){self.__ck.push({ev:ci});});}"
                "catch(e){}\n");
        w.close();
    }

    ExtensionManager *manager = ExtensionManager::instance();
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();
    // Leftovers are persistent across standalone runs — drop any
    // stale 'tst-cookies' registration or loadExtension no-ops.
    for (const ExtensionManager::ExtensionInfo &info
         : manager->extensions()) {
        if (info.name == QLatin1String("tst-cookies"))
            manager->removeExtension(info.id);
    }
    auto loaded = std::make_shared<QString>();
    auto loadErr = std::make_shared<QString>();
    QObject::connect(manager, &ExtensionManager::extensionLoaded,
                     this, [loaded, loadErr](
                         const ExtensionManager::ExtensionInfo &info) {
        if (info.name != QLatin1String("tst-cookies"))
            return;
        if (info.error.isEmpty())
            *loaded = info.id;
        else
            *loadErr = info.error;
    });
    manager->grantConsent(dir);
    manager->loadExtension(dir);
    QElapsedTimer timer;
    timer.start();
    while (loaded->isEmpty() && loadErr->isEmpty()
           && timer.elapsed() < 30000)
        QTest::qWait(50);
    const QString extId = *loaded;
    QVERIFY2(!extId.isEmpty(), qPrintable(QStringLiteral(
        "fixture loaded (err=%1)").arg(*loadErr)));
    manager->setExtensionEnabled(extId, true);
    auto unload = qScopeGuard([manager, extId, dir]() {
        manager->setExtensionEnabled(extId, false);
        manager->removeExtension(extId);
        QDir(dir).removeRecursively();
    });
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime->host(extId)
            && runtime->host(extId)->state()
                   == ExtensionBackgroundHost::State::Ready,
        45000);

    // Same ambient-persona binding the MD22 leg uses — the runtime's
    // permission/coverage maps are already filled from the manifest,
    // so the switch only re-points which jar invokeCookies serves.
    const QString prevId = BrowserPaths::activeProfileId();
    const QString profileId = QStringLiteral("tstextck%1")
            .arg(QCoreApplication::applicationPid());
    BrowserPaths::setActiveProfileId(profileId);
    CookieJar *jar = CookieJar::instanceFor(profileId);
    QVERIFY(jar);
    QCOMPARE(jar->profileId(), profileId);
    QVERIFY(jar->profile() && jar->profile()->cookieStore());
    // The cookie store is backed by the network service, which only
    // exists once a page does (same bring-up cookieStoreIsolation
    // documents) — a trivial load spins the engine up so the live
    // setCookie leg below actually lands.
    {
        QWebEnginePage spinUp(jar->profile());
        QSignalSpy loadSpy(&spinUp, &QWebEnginePage::loadFinished);
        spinUp.load(QUrl(QStringLiteral("about:blank")));
        QTRY_VERIFY_WITH_TIMEOUT(loadSpy.count() >= 1, 15000);
    }
    auto unbind = qScopeGuard([prevId, profileId]() {
        CookieJar::releaseFor(profileId);
        BrowserPaths::setActiveProfileId(prevId);
        QDir(BrowserPaths::profileDataDir(profileId))
            .removeRecursively();
    });

    ChromeBridge *bridge = runtime->host(extId)->bridge();
    QVERIFY(bridge);
    QSignalSpy hostEvents(bridge, &ChromeBridge::hostEvent);
    auto invoke = [&](const QString &m, const QJsonObject &args) {
        return bridge->invoke(QStringLiteral("cookies"), m, args);
    };

    // NOTE: the runtime's hostCovers wildcard is a single path
    // segment ('*' does not cross '/'), so the covered url is the
    // host root.
    const QString url =
        QStringLiteral("https://www.example.test/");
    auto hostEventsFor = [&](const QString &name) {
        QList<QVariantList> out;
        for (const QVariantList &args : hostEvents)
            if (!args.isEmpty() && args.at(0).toString() == name)
                out.append(args);
        return out;
    };

    // set on a covered host — Chrome's Cookie shape answers with the
    // store id; the record lands in the mirror synchronously.
    QJsonObject reply = invoke(QStringLiteral("set"), QJsonObject{
        {QStringLiteral("url"), url},
        {QStringLiteral("name"), QStringLiteral("sid")},
        {QStringLiteral("value"), QStringLiteral("v1")},
        {QStringLiteral("path"), QStringLiteral("/")},
        {QStringLiteral("secure"), true},
        {QStringLiteral("httpOnly"), true},
        {QStringLiteral("sameSite"), QStringLiteral("lax")}});
    QVERIFY2(reply.value(QLatin1String("ok")).toBool(),
             qPrintable(reply.value(QLatin1String("error")).toString()));
    {
        const QJsonObject c =
            reply.value(QLatin1String("result")).toObject();
        QCOMPARE(c.value(QLatin1String("name")).toString(),
                 QStringLiteral("sid"));
        QCOMPARE(c.value(QLatin1String("domain")).toString(),
                 QStringLiteral("www.example.test"));
        QCOMPARE(c.value(QLatin1String("hostOnly")).toBool(), true);
        QCOMPARE(c.value(QLatin1String("path")).toString(),
                 QStringLiteral("/"));
        QCOMPARE(c.value(QLatin1String("secure")).toBool(), true);
        QCOMPARE(c.value(QLatin1String("httpOnly")).toBool(), true);
        QCOMPARE(c.value(QLatin1String("sameSite")).toString(),
                 QStringLiteral("lax"));
        QCOMPARE(c.value(QLatin1String("session")).toBool(), true);
        QCOMPARE(c.value(QLatin1String("storeId")).toString(),
                 QStringLiteral("0"));
    }

    // The engine store took the live write (lands asynchronously).
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        const QList<QNetworkCookie> cs = jar->cookies();
        for (const QNetworkCookie &c : cs)
            if (c.name() == "sid" && c.value() == "v1")
                return true;
        return false;
    }(), 15000);

    // get — mirror read answers the same row; a miss answers null.
    reply = invoke(QStringLiteral("get"), QJsonObject{
        {QStringLiteral("url"), url},
        {QStringLiteral("name"), QStringLiteral("sid")}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QCOMPARE(reply.value(QLatin1String("result"))
                 .toObject().value(QLatin1String("value")).toString(),
             QStringLiteral("v1"));
    reply = invoke(QStringLiteral("get"), QJsonObject{
        {QStringLiteral("url"), url},
        {QStringLiteral("name"), QStringLiteral("absent")}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QVERIFY(reply.value(QLatin1String("result")).isNull()
            || reply.value(QLatin1String("result")).isUndefined());

    // getAll — the row answers under the store filter and the url
    // send-match alike; unknown store id is Chrome's empty answer.
    reply = invoke(QStringLiteral("getAll"), QJsonObject{
        {QStringLiteral("domain"),
         QStringLiteral("example.test")}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QVERIFY(reply.value(QLatin1String("result")).toArray().size() >= 1);
    reply = invoke(QStringLiteral("getAll"), QJsonObject{
        {QStringLiteral("url"), url},
        {QStringLiteral("name"), QStringLiteral("sid")}});
    QCOMPARE(reply.value(QLatin1String("result")).toArray().size(), 1);
    reply = invoke(QStringLiteral("getAll"), QJsonObject{
        {QStringLiteral("storeId"), QStringLiteral("zzz")}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QVERIFY(reply.value(QLatin1String("result")).toArray().isEmpty());

    // getAllCookieStores — one served store, Chrome's {id,tabIds}.
    reply = invoke(QStringLiteral("getAllCookieStores"), QJsonObject());
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    {
        const QJsonArray stores =
            reply.value(QLatin1String("result")).toArray();
        QCOMPARE(stores.size(), 1);
        QCOMPARE(stores.at(0).toObject()
                     .value(QLatin1String("id")).toString(),
                 QStringLiteral("0"));
        QVERIFY(stores.at(0).toObject()
                .value(QLatin1String("tabIds")).isArray());
    }

    // onChanged — the engine's cookieAdded rode the tap into the
    // hosted context: removed:false, cause 'explicit', the Cookie.
    QTRY_VERIFY_WITH_TIMEOUT(
        !hostEventsFor(QStringLiteral("cookies.onChanged")).isEmpty(),
        15000);
    {
        const QVariantList last =
            hostEventsFor(QStringLiteral("cookies.onChanged")).last();
        const QJsonObject ci = last.at(1).toJsonObject()
            .value(QLatin1String("__args")).toArray().at(0).toObject();
        QCOMPARE(ci.value(QLatin1String("removed")).toBool(), false);
        QCOMPARE(ci.value(QLatin1String("cookie")).toObject()
                     .value(QLatin1String("name")).toString(),
                 QStringLiteral("sid"));
    }

    // Host coverage — an uncovered domain fails url-bearing ops with
    // the typed verdict; a covered op still answers.
    QCOMPARE(invoke(QStringLiteral("get"), QJsonObject{
                 {QStringLiteral("url"),
                  QStringLiteral("https://uncovered.test/x")},
                 {QStringLiteral("name"), QStringLiteral("a")}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("no-host-access"));
    QCOMPARE(invoke(QStringLiteral("set"), QJsonObject{
                 {QStringLiteral("url"),
                  QStringLiteral("https://uncovered.test/")},
                 {QStringLiteral("name"), QStringLiteral("a")}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("no-host-access"));

    // The onChanged withhold: an engine-side change on an uncovered
    // domain reaches neither the mirror-served context's event list.
    {
        hostEvents.clear();
        QNetworkCookie foreign(QStringLiteral("x").toUtf8(),
                               QStringLiteral("1").toUtf8());
        foreign.setDomain(QStringLiteral("uncovered.test"));
        foreign.setPath(QStringLiteral("/"));
        jar->profile()->cookieStore()->setCookie(
            foreign, QUrl(QStringLiteral("https://uncovered.test/")));
        QTest::qWait(1200);
        QVERIFY(hostEventsFor(QStringLiteral("cookies.onChanged"))
                    .isEmpty());
    }

    // remove — the cookie get() would return goes away in the live
    // jar AND the mirror; the reply is Chrome's details echo.
    reply = invoke(QStringLiteral("remove"), QJsonObject{
        {QStringLiteral("url"), url},
        {QStringLiteral("name"), QStringLiteral("sid")}});
    QVERIFY2(reply.value(QLatin1String("ok")).toBool(),
             qPrintable(reply.value(QLatin1String("error")).toString()));
    QCOMPARE(reply.value(QLatin1String("result")).toObject()
                 .value(QLatin1String("name")).toString(),
             QStringLiteral("sid"));
    QCOMPARE(reply.value(QLatin1String("result")).toObject()
                 .value(QLatin1String("storeId")).toString(),
             QStringLiteral("0"));
    reply = invoke(QStringLiteral("get"), QJsonObject{
        {QStringLiteral("url"), url},
        {QStringLiteral("name"), QStringLiteral("sid")}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QVERIFY(reply.value(QLatin1String("result")).isNull()
            || reply.value(QLatin1String("result")).isUndefined());
    QTRY_VERIFY_WITH_TIMEOUT(
        !hostEventsFor(QStringLiteral("cookies.onChanged")).isEmpty(),
        15000);
    {
        const QVariantList last =
            hostEventsFor(QStringLiteral("cookies.onChanged")).last();
        const QJsonObject ci = last.at(1).toJsonObject()
            .value(QLatin1String("__args")).toArray().at(0).toObject();
        QCOMPARE(ci.value(QLatin1String("removed")).toBool(), true);
    }

    // Refused shapes — missing url/name, non-http(s) set urls.
    QCOMPARE(invoke(QStringLiteral("get"), QJsonObject())
             .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    QCOMPARE(invoke(QStringLiteral("remove"), QJsonObject{
                 {QStringLiteral("url"), url}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    QCOMPARE(invoke(QStringLiteral("set"), QJsonObject{
                 {QStringLiteral("url"),
                  QStringLiteral("file:///x")},
                 {QStringLiteral("name"), QStringLiteral("a")}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("scheme-refused"));
    QCOMPARE(invoke(QStringLiteral("bogus"), QJsonObject())
             .value(QLatin1String("error")).toString(),
             QStringLiteral("unimplemented"));

    // The manifest gate — a sibling backend without 'cookies' hears
    // no-permission on every member.
    const QString dirB = QDir::temp().filePath(
        QStringLiteral("tst-extckb-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dirB));
    {
        QFile f(dirB + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-ck-b\","
                "\"version\":\"1.0\",\"permissions\":[\"storage\"]}");
        f.close();
    }
    ExtensionShimBackend backendB(
        QStringLiteral("tstmockextensionid0031"),
        QStringLiteral("tst-ck-b"), dirB);
    QCOMPARE(backendB.invoke(QStringLiteral("cookies"),
                 QStringLiteral("get"), QJsonObject{
                     {QStringLiteral("url"), url},
                     {QStringLiteral("name"), QStringLiteral("a")}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("no-permission"));
    QCOMPARE(backendB.invoke(QStringLiteral("cookies"),
                 QStringLiteral("getAll"), QJsonObject())
             .value(QLatin1String("error")).toString(),
             QStringLiteral("no-permission"));

    QDir(dirB).removeRecursively();
}

// MD24 — chrome.downloads over DownloadManager's record list.  A
// REAL hosted extension ('downloads' grant) drives bridge->invoke so
// the eventGate + relay fan-out run for onCreated/onChanged/onErased;
// the deferred '__arktos.downloadCreated' push carries the minted id.
// A dedicated temp persona binds the ambient DownloadManager, the
// builtin engine is pinned (rustdl would claim the http fixture), and
// the verify leg rides a stalled loopback server: pause/resume/cancel
// on an in-flight row, search, erase+onErased, the typed verdicts and
// a bare-backend ticket push.
void tst_ExtensionManager::extensionDownloads()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");

    // Fixture: downloads grant + a background context so the runtime
    // registers a relay for the event fan-out.
    const QString dir = QDir::temp().filePath(
        QStringLiteral("tst-extdl-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dir));
    {
        QFile f(dir + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-dl\","
                "\"version\":\"1.0\",\"permissions\":[\"downloads\"],"
                "\"background\":{\"service_worker\":\"sw.js\"}}");
        f.close();
        QFile w(dir + QLatin1String("/sw.js"));
        QVERIFY(w.open(QIODevice::WriteOnly));
        w.write("self.__dl=[];\n"
                "try{chrome.downloads.onCreated.addListener("
                "function(i){self.__dl.push(i);});}catch(e){}\n"
                "try{chrome.downloads.onChanged.addListener("
                "function(d){self.__dl.push(d);});}catch(e){}\n"
                "try{chrome.downloads.onErased.addListener("
                "function(i){self.__dl.push(i);});}catch(e){}\n");
        w.close();
    }

    ExtensionManager *manager = ExtensionManager::instance();
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();
    for (const ExtensionManager::ExtensionInfo &info
         : manager->extensions()) {
        if (info.name == QLatin1String("tst-dl"))
            manager->removeExtension(info.id);
    }
    auto loaded = std::make_shared<QString>();
    auto loadErr = std::make_shared<QString>();
    QObject::connect(manager, &ExtensionManager::extensionLoaded,
                     this, [loaded, loadErr](
                         const ExtensionManager::ExtensionInfo &info) {
        if (info.name != QLatin1String("tst-dl"))
            return;
        if (info.error.isEmpty())
            *loaded = info.id;
        else
            *loadErr = info.error;
    });
    manager->grantConsent(dir);
    manager->loadExtension(dir);
    QElapsedTimer timer;
    timer.start();
    while (loaded->isEmpty() && loadErr->isEmpty()
           && timer.elapsed() < 30000)
        QTest::qWait(50);
    const QString extId = *loaded;
    QVERIFY2(!extId.isEmpty(), qPrintable(QStringLiteral(
        "fixture loaded (err=%1)").arg(*loadErr)));
    manager->setExtensionEnabled(extId, true);
    auto unload = qScopeGuard([manager, extId, dir]() {
        manager->setExtensionEnabled(extId, false);
        manager->removeExtension(extId);
        QDir(dir).removeRecursively();
    });
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime->host(extId)
            && runtime->host(extId)->state()
                   == ExtensionBackgroundHost::State::Ready,
        45000);

    // Ambient-persona binding — invokeDownloads resolves exactly this
    // manager's record list.
    const QString prevId = BrowserPaths::activeProfileId();
    const QString profileId = QStringLiteral("tstextdl%1")
            .arg(QCoreApplication::applicationPid());
    BrowserPaths::setActiveProfileId(profileId);
    // Same pin the downloadmanager autotests apply — the rust
    // selector defaults to claiming http(s) urls on rustdl builds;
    // this leg proves the Chromium request path.  The claim gate
    // reads the ACTIVE persona's scope, so the key must land on the
    // freshly-bound profile, not the ambient one.
    ProfileSettings settings(profileId);
    settings.setValue(QLatin1String("downloadmanager/engine"),
                      QLatin1String("builtin"));
    DownloadManager *dlm = DownloadManager::instanceFor(profileId);
    QVERIFY(dlm);
    QCOMPARE(dlm->profileId(), profileId);
    QTemporaryDir downloadDir;
    QVERIFY(downloadDir.isValid());
    dlm->setDownloadDirectory(downloadDir.path() + QLatin1Char('/'));
    auto unbind = qScopeGuard([prevId, profileId]() {
        DownloadManager::releaseFor(profileId);
        BrowserPaths::setActiveProfileId(prevId);
        QDir(BrowserPaths::profileDataDir(profileId))
            .removeRecursively();
    });

    // A stalled loopback download — headers promise a huge body the
    // trickle never finishes, so the row stays in-flight for the
    // pause/resume/cancel leg (the trick the downloadmanager
    // autotests use: Chromium only raises downloadRequested once
    // body bytes arrive).
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    connect(&server, &QTcpServer::newConnection, &server, [&server]() {
        QTcpSocket *socket = server.nextPendingConnection();
        socket->setParent(&server);
        socket->readAll();
        socket->write("HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/octet-stream\r\n"
                      "Content-Disposition: attachment; "
                      "filename=\"stall.bin\"\r\n"
                      "Content-Length: 104857600\r\n"
                      "\r\n");
        QTimer *trickle = new QTimer(socket);
        QObject::connect(trickle, &QTimer::timeout, socket,
                         [socket]() { socket->write("x"); });
        trickle->start(200);
    });
    const QString stalledUrl = QString::fromLatin1(
        "http://127.0.0.1:%1/stall.bin").arg(server.serverPort());
    const QString dataUrl = QStringLiteral(
        "data:text/plain;base64,")
        + QString::fromLatin1(QByteArray("hello world").toBase64());

    ChromeBridge *bridge = runtime->host(extId)->bridge();
    QVERIFY(bridge);
    QSignalSpy hostEvents(bridge, &ChromeBridge::hostEvent);
    auto invoke = [&](const QString &m, const QJsonObject &args) {
        return bridge->invoke(QStringLiteral("downloads"), m, args);
    };
    auto hostEventsFor = [&](const QString &name) {
        QList<QVariantList> out;
        for (const QVariantList &args : hostEvents)
            if (!args.isEmpty() && args.at(0).toString() == name)
                out.append(args);
        return out;
    };

    // Empty list — search answers the empty array; the shelf surface
    // and unknown members stay typed.
    QJsonObject reply = invoke(QStringLiteral("search"), QJsonObject{
        {QStringLiteral("query"), QJsonObject()}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QVERIFY(reply.value(QLatin1String("result")).toArray().isEmpty());
    QCOMPARE(invoke(QStringLiteral("setShelfEnabled"), QJsonObject{
                 {QStringLiteral("enabled"), true}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("unsupported"));
    QCOMPARE(invoke(QStringLiteral("drag"), QJsonObject{
                 {QStringLiteral("id"), 1.0}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("unsupported"));
    QCOMPARE(invoke(QStringLiteral("bogus"), QJsonObject())
             .value(QLatin1String("error")).toString(),
             QStringLiteral("unimplemented"));

    // download() — the synchronous answer is a ticket; the Chrome
    // download id arrives via the '__arktos.downloadCreated' push.
    reply = invoke(QStringLiteral("download"), QJsonObject{
        {QStringLiteral("url"), stalledUrl}});
    QVERIFY2(reply.value(QLatin1String("ok")).toBool(),
             qPrintable(reply.value(QLatin1String("error")).toString()));
    const int ticket =
        reply.value(QLatin1String("result")).toObject()
            .value(QLatin1String("ticket")).toInt();
    QVERIFY(ticket > 0);
    QTRY_VERIFY_WITH_TIMEOUT(
        !hostEventsFor(QStringLiteral("__arktos.downloadCreated"))
             .isEmpty(),
        30000);
    int dlId = -1;
    {
        const QVariantList pushed =
            hostEventsFor(QStringLiteral("__arktos.downloadCreated"))
                .last();
        const QJsonObject body = pushed.at(1).toJsonObject();
        QCOMPARE(body.value(QLatin1String("ticket")).toInt(), ticket);
        QVERIFY(body.value(QLatin1String("error")).isUndefined());
        dlId = body.value(QLatin1String("id")).toInt();
    }
    QVERIFY(dlId > 0);

    // downloads.onCreated rode the itemAdded tap to the relay — the
    // record answers Chrome's DownloadItem shape with byExtId set.
    QTRY_VERIFY_WITH_TIMEOUT(
        !hostEventsFor(QStringLiteral("downloads.onCreated"))
             .isEmpty(),
        15000);
    {
        const QVariantList created =
            hostEventsFor(QStringLiteral("downloads.onCreated")).last();
        const QJsonObject item = created.at(1).toJsonObject()
            .value(QLatin1String("__args")).toArray().at(0).toObject();
        QCOMPARE(item.value(QLatin1String("id")).toInt(), dlId);
        QCOMPARE(item.value(QLatin1String("url")).toString(),
                 stalledUrl);
        QCOMPARE(item.value(QLatin1String("byExtId")).toString(),
                 extId);
    }

    // search — id and url filters both land the same in-flight row.
    reply = invoke(QStringLiteral("search"), QJsonObject{
        {QStringLiteral("query"), QJsonObject{
            {QStringLiteral("id"), dlId}}}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    {
        const QJsonArray hits =
            reply.value(QLatin1String("result")).toArray();
        QCOMPARE(hits.size(), 1);
        const QJsonObject item = hits.at(0).toObject();
        QCOMPARE(item.value(QLatin1String("state")).toString(),
                 QStringLiteral("in_progress"));
        QCOMPARE(item.value(QLatin1String("paused")).toBool(), false);
        QCOMPARE(item.value(QLatin1String("incognito")).toBool(),
                 false);
        QVERIFY(!item.value(QLatin1String("filename")).toString()
                     .isEmpty());
    }
    reply = invoke(QStringLiteral("search"), QJsonObject{
        {QStringLiteral("query"), QJsonObject{
            {QStringLiteral("url"), QStringLiteral("stall.bin")}}}});
    QCOMPARE(reply.value(QLatin1String("result")).toArray().size(), 1);

    // pause -> onChanged delta {paused}, resume -> {paused:false};
    // open/removeFile refuse a row that never completed.  The card's
    // strict pausable gate is a live request in InProgress — wait
    // for the trickle's first byte so the engine has actually
    // started (accept→InProgress lags the item's minting).
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        const QJsonObject r = invoke(
            QStringLiteral("search"), QJsonObject{
                {QStringLiteral("query"), QJsonObject{
                    {QStringLiteral("id"), dlId}}}});
        const QJsonArray hits =
            r.value(QLatin1String("result")).toArray();
        return !hits.isEmpty()
            && hits.at(0).toObject()
                       .value(QLatin1String("bytesReceived"))
                       .toDouble() > 0;
    }(), 15000);
    reply = invoke(QStringLiteral("pause"), QJsonObject{
        {QStringLiteral("id"), dlId}});
    QVERIFY2(reply.value(QLatin1String("ok")).toBool(),
             qPrintable(reply.value(QLatin1String("error")).toString()));
    // QtWebEngine applies a pause asynchronously — the honest
    // {paused:true} delta rides the request's isPausedChanged tap.
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        const QList<QVariantList> deltas =
            hostEventsFor(QStringLiteral("downloads.onChanged"));
        for (const QVariantList &d : deltas) {
            const QJsonObject delta = d.at(1).toJsonObject()
                .value(QLatin1String("__args")).toArray()
                .at(0).toObject();
            if (delta.value(QLatin1String("id")).toInt() == dlId
                    && delta.value(QLatin1String("paused")).toObject()
                           .value(QLatin1String("current")).toBool())
                return true;
        }
        return false;
    }(), 15000);
    reply = invoke(QStringLiteral("resume"), QJsonObject{
        {QStringLiteral("id"), dlId}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QCOMPARE(invoke(QStringLiteral("open"), QJsonObject{
                 {QStringLiteral("id"), dlId}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("not-complete"));
    QCOMPARE(invoke(QStringLiteral("removeFile"), QJsonObject{
                 {QStringLiteral("id"), dlId}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("not-complete"));

    // cancel — the card stops, onChanged carries the terminal delta,
    // the row answers 'interrupted' with Chrome's USER_CANCELED.
    reply = invoke(QStringLiteral("cancel"), QJsonObject{
        {QStringLiteral("id"), dlId}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        const QList<QVariantList> deltas =
            hostEventsFor(QStringLiteral("downloads.onChanged"));
        for (const QVariantList &d : deltas) {
            const QJsonObject delta = d.at(1).toJsonObject()
                .value(QLatin1String("__args")).toArray()
                .at(0).toObject();
            if (delta.value(QLatin1String("id")).toInt() == dlId
                    && delta.value(QLatin1String("state")).toObject()
                               .value(QLatin1String("current"))
                               .toString()
                           == QLatin1String("interrupted"))
                return true;
        }
        return false;
    }(), 15000);
    reply = invoke(QStringLiteral("search"), QJsonObject{
        {QStringLiteral("query"), QJsonObject{
            {QStringLiteral("id"), dlId}}}});
    {
        const QJsonObject item = reply.value(QLatin1String("result"))
            .toArray().at(0).toObject();
        QCOMPARE(item.value(QLatin1String("state")).toString(),
                 QStringLiteral("interrupted"));
        QCOMPARE(item.value(QLatin1String("error")).toString(),
                 QStringLiteral("USER_CANCELED"));
    }
    // Cancelling a finished row is the typed refusal.
    QCOMPARE(invoke(QStringLiteral("cancel"), QJsonObject{
                 {QStringLiteral("id"), dlId}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("not-in-progress"));

    // erase — the row leaves the record list; the model tap fires
    // onErased with the Chrome download id.
    reply = invoke(QStringLiteral("erase"), QJsonObject{
        {QStringLiteral("query"), QJsonObject{
            {QStringLiteral("id"), dlId}}}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QTRY_VERIFY_WITH_TIMEOUT(
        !hostEventsFor(QStringLiteral("downloads.onErased")).isEmpty(),
        15000);
    QCOMPARE(hostEventsFor(QStringLiteral("downloads.onErased")).last()
                 .at(1).toJsonObject()
                 .value(QLatin1String("__args")).toArray().at(0).toInt(),
             dlId);
    reply = invoke(QStringLiteral("search"), QJsonObject{
        {QStringLiteral("query"), QJsonObject{
            {QStringLiteral("id"), dlId}}}});
    QVERIFY(reply.value(QLatin1String("result")).toArray().isEmpty());

    // Refused shapes — missing url/query/id, bad scheme, the POST
    // and 'overwrite' options Arktos cannot honour.
    QCOMPARE(invoke(QStringLiteral("download"), QJsonObject())
             .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    QCOMPARE(invoke(QStringLiteral("download"), QJsonObject{
                 {QStringLiteral("url"),
                  QStringLiteral("javascript:alert(1)")}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("scheme-refused"));
    QCOMPARE(invoke(QStringLiteral("download"), QJsonObject{
                 {QStringLiteral("url"), stalledUrl},
                 {QStringLiteral("method"), QStringLiteral("POST")}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("unsupported"));
    QCOMPARE(invoke(QStringLiteral("download"), QJsonObject{
                 {QStringLiteral("url"), stalledUrl},
                 {QStringLiteral("conflictAction"),
                  QStringLiteral("overwrite")}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("unsupported"));
    QCOMPARE(invoke(QStringLiteral("search"), QJsonObject())
             .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    QCOMPARE(invoke(QStringLiteral("pause"), QJsonObject())
             .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    QCOMPARE(invoke(QStringLiteral("pause"), QJsonObject{
                 {QStringLiteral("id"), 999999.0}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("not-found"));

    // A bare backend (no relay context) — the ticket answer falls
    // back to pushEvent; a data: download completes so getFileIcon
    // and removeFile can answer their real verdicts.
    const QString dirB = QDir::temp().filePath(
        QStringLiteral("tst-extdlb-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dirB));
    {
        QFile f(dirB + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-dl-b\","
                "\"version\":\"1.0\",\"permissions\":[\"downloads\"]}");
        f.close();
    }
    ExtensionShimBackend backendB(
        QStringLiteral("tstmockextensionid0041"),
        QStringLiteral("tst-dl-b"), dirB);
    QSignalSpy pushSpy(&backendB, &ExtensionShimBackend::pushEvent);
    reply = backendB.invoke(QStringLiteral("downloads"),
                            QStringLiteral("download"), QJsonObject{
        {QStringLiteral("url"), dataUrl},
        {QStringLiteral("filename"),
         QStringLiteral("md24-fixture.txt")}});
    QVERIFY2(reply.value(QLatin1String("ok")).toBool(),
             qPrintable(reply.value(QLatin1String("error")).toString()));
    const int ticketB =
        reply.value(QLatin1String("result")).toObject()
            .value(QLatin1String("ticket")).toInt();
    QVERIFY(ticketB > 0);
    int dlIdB = -1;
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        for (const QVariantList &push : pushSpy) {
            if (push.isEmpty()
                    || push.at(0).toString()
                           != QLatin1String("__arktos.downloadCreated"))
                continue;
            const QJsonObject body = push.at(1).toJsonObject();
            if (body.value(QLatin1String("ticket")).toInt() != ticketB)
                continue;
            dlIdB = body.value(QLatin1String("id")).toInt();
            return true;
        }
        return false;
    }(), 30000);
    QVERIFY(dlIdB > 0);

    // The suggested filename landed under the download directory.
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        const QJsonObject r = backendB.invoke(
            QStringLiteral("downloads"), QStringLiteral("search"),
            QJsonObject{{QStringLiteral("query"), QJsonObject{
                {QStringLiteral("id"), dlIdB}}}});
        const QJsonArray hits =
            r.value(QLatin1String("result")).toArray();
        if (hits.isEmpty())
            return false;
        const QJsonObject item = hits.at(0).toObject();
        return item.value(QLatin1String("filename")).toString()
                   .endsWith(QLatin1String("md24-fixture.txt"));
    }(), 15000);

    // Wait for the tiny transfer to finish, then the completed-row
    // members answer: getFileIcon's data url, removeFile's real
    // unlink (exists flips false), open's typed file-missing.
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        const QJsonObject r = backendB.invoke(
            QStringLiteral("downloads"), QStringLiteral("search"),
            QJsonObject{{QStringLiteral("query"), QJsonObject{
                {QStringLiteral("id"), dlIdB}}}});
        const QJsonArray hits =
            r.value(QLatin1String("result")).toArray();
        return !hits.isEmpty()
            && hits.at(0).toObject()
                       .value(QLatin1String("state")).toString()
                   == QLatin1String("complete");
    }(), 15000);
    reply = backendB.invoke(QStringLiteral("downloads"),
                            QStringLiteral("getFileIcon"), QJsonObject{
        {QStringLiteral("id"), dlIdB},
        {QStringLiteral("options"), QJsonObject{
            {QStringLiteral("size"), 16}}}});
    QVERIFY2(reply.value(QLatin1String("ok")).toBool(),
             qPrintable(reply.value(QLatin1String("error")).toString()));
    QVERIFY(reply.value(QLatin1String("result")).toString()
                .startsWith(QLatin1String("data:image/png;base64,")));
    reply = backendB.invoke(QStringLiteral("downloads"),
                            QStringLiteral("removeFile"), QJsonObject{
        {QStringLiteral("id"), dlIdB}});
    QVERIFY2(reply.value(QLatin1String("ok")).toBool(),
             qPrintable(reply.value(QLatin1String("error")).toString()));
    reply = backendB.invoke(QStringLiteral("downloads"),
                            QStringLiteral("search"), QJsonObject{
        {QStringLiteral("query"), QJsonObject{
            {QStringLiteral("id"), dlIdB}}}});
    QCOMPARE(reply.value(QLatin1String("result")).toArray().at(0)
                 .toObject().value(QLatin1String("exists")).toBool(),
             false);
    QCOMPARE(backendB.invoke(QStringLiteral("downloads"),
                 QStringLiteral("open"), QJsonObject{
                     {QStringLiteral("id"), dlIdB}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("file-missing"));

    // showDefaultFolder — offscreen has no file-manager channel; the
    // typed refusal is the honest answer here (ok on a real desktop).
    reply = backendB.invoke(QStringLiteral("downloads"),
                            QStringLiteral("showDefaultFolder"),
                            QJsonObject());
    if (!reply.value(QLatin1String("ok")).toBool())
        QCOMPARE(reply.value(QLatin1String("error")).toString(),
                 QStringLiteral("reveal-failed"));

    // The manifest gate — a sibling without 'downloads' hears
    // no-permission on every member.
    const QString dirC = QDir::temp().filePath(
        QStringLiteral("tst-extdlc-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dirC));
    {
        QFile f(dirC + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-dl-c\","
                "\"version\":\"1.0\",\"permissions\":[\"storage\"]}");
        f.close();
    }
    ExtensionShimBackend backendC(
        QStringLiteral("tstmockextensionid0042"),
        QStringLiteral("tst-dl-c"), dirC);
    QCOMPARE(backendC.invoke(QStringLiteral("downloads"),
                 QStringLiteral("search"), QJsonObject{
                     {QStringLiteral("query"), QJsonObject()}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("no-permission"));
    QCOMPARE(backendC.invoke(QStringLiteral("downloads"),
                 QStringLiteral("download"), QJsonObject{
                     {QStringLiteral("url"), dataUrl}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("no-permission"));

    QDir(dirB).removeRecursively();
    QDir(dirC).removeRecursively();
}

// MD25 — chrome.sessions over TabWidget's TABS05 closed-tab undo
// stack.  A hosted fixture ('sessions' grant + a background context
// that registers onChanged and snapshots MAX_SESSION_RESULTS)
// drives bridge->invoke: getDevices answers the honest empty list,
// getRecentlyClosed enumerates a real window's undo record after
// closeTab, restore() rebuilds it through the shared undo path, and
// sessions.onChanged rides the hub's recentlyClosedTabsChanged tap.
// The manifest gate lands on a bare sibling backend.
void tst_ExtensionManager::extensionSessions()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");

    const QString dir = QDir::temp().filePath(
        QStringLiteral("tst-extsess-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dir));
    {
        QFile f(dir + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-sess\","
                "\"version\":\"1.0\",\"permissions\":[\"sessions\"],"
                "\"background\":{\"service_worker\":\"sw.js\"}}");
        f.close();
        QFile w(dir + QLatin1String("/sw.js"));
        QVERIFY(w.open(QIODevice::WriteOnly));
        w.write("self.__sess=[];\n"
                "self.__sessMax=(chrome.sessions"
                " ? chrome.sessions.MAX_SESSION_RESULTS : null);\n"
                "try{chrome.sessions.onChanged.addListener("
                "function(){self.__sess.push('changed');});}"
                "catch(e){}\n");
        w.close();
    }

    ExtensionManager *manager = ExtensionManager::instance();
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();
    for (const ExtensionManager::ExtensionInfo &info
         : manager->extensions()) {
        if (info.name == QLatin1String("tst-sess"))
            manager->removeExtension(info.id);
    }
    auto loaded = std::make_shared<QString>();
    auto loadErr = std::make_shared<QString>();
    QObject::connect(manager, &ExtensionManager::extensionLoaded,
                     this, [loaded, loadErr](
                         const ExtensionManager::ExtensionInfo &info) {
        if (info.name != QLatin1String("tst-sess"))
            return;
        if (info.error.isEmpty())
            *loaded = info.id;
        else
            *loadErr = info.error;
    });
    manager->grantConsent(dir);
    manager->loadExtension(dir);
    QElapsedTimer timer;
    timer.start();
    while (loaded->isEmpty() && loadErr->isEmpty()
           && timer.elapsed() < 30000)
        QTest::qWait(50);
    const QString extId = *loaded;
    QVERIFY2(!extId.isEmpty(), qPrintable(QStringLiteral(
        "fixture loaded (err=%1)").arg(*loadErr)));
    manager->setExtensionEnabled(extId, true);
    auto unload = qScopeGuard([manager, extId, dir]() {
        manager->setExtensionEnabled(extId, false);
        manager->removeExtension(extId);
        QDir(dir).removeRecursively();
    });
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime->host(extId)
            && runtime->host(extId)->state()
                   == ExtensionBackgroundHost::State::Ready,
        45000);

    ChromeBridge *bridge = runtime->host(extId)->bridge();
    QVERIFY(bridge);
    QSignalSpy hostEvents(bridge, &ChromeBridge::hostEvent);
    auto invoke = [&](const QString &m, const QJsonObject &args) {
        return bridge->invoke(QStringLiteral("sessions"), m, args);
    };
    auto hostEventsFor = [&](const QString &name) {
        QList<QVariantList> out;
        for (const QVariantList &args : hostEvents)
            if (!args.isEmpty() && args.at(0).toString() == name)
                out.append(args);
        return out;
    };
    auto eval = [](QWebEnginePage *page, const QString &source) {
        auto result = std::make_shared<QVariant>();
        page->runJavaScript(source,
                            [result](const QVariant &v) { *result = v; });
        QElapsedTimer t;
        t.start();
        while (!result->isValid() && t.elapsed() < 10000)
            QTest::qWait(50);
        return *result;
    };

    // The namespace constant landed page-side.
    QCOMPARE(eval(runtime->host(extId)->page(),
                  QStringLiteral("self.__sessMax")).toInt(), 25);

    // Empty-stack answers + the arg refusals that don't need a record.
    QJsonObject reply = invoke(QStringLiteral("getDevices"),
                               QJsonObject());
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QVERIFY(reply.value(QLatin1String("result")).toArray().isEmpty());
    reply = invoke(QStringLiteral("getRecentlyClosed"),
                   QJsonObject());
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QVERIFY(reply.value(QLatin1String("result")).toArray().isEmpty());
    QCOMPARE(invoke(QStringLiteral("restore"), QJsonObject())
             .value(QLatin1String("error")).toString(),
             QStringLiteral("not-found"));
    QCOMPARE(invoke(QStringLiteral("getRecentlyClosed"), QJsonObject{
                 {QStringLiteral("filter"), QJsonObject{
                     {QStringLiteral("maxResults"), 26.0}}}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    reply = invoke(QStringLiteral("getRecentlyClosed"), QJsonObject{
        {QStringLiteral("filter"), QJsonObject{
            {QStringLiteral("maxResults"), 0.0}}}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QVERIFY(reply.value(QLatin1String("result")).toArray().isEmpty());
    QCOMPARE(invoke(QStringLiteral("bogus"), QJsonObject())
             .value(QLatin1String("error")).toString(),
             QStringLiteral("unimplemented"));

    // A real window + a closable tab — the undo record must carry a
    // url, so the tab loads a fixture document first.
    ExtensionEventHub *hub = ExtensionEventHub::instance();
    hub->setTracking(true);
    hub->syncNow();
    BrowserMainWindow *window = new BrowserMainWindow;
    TabWidget *tw = window->tabWidget();
    QVERIFY(tw && tw->count() >= 1);
    hub->syncNow();   // wires recentlyClosedTabsChanged on this strip
    tw->newTab();
    QCOMPARE(tw->count(), 2);
    WebView *view = tw->webView(1);
    QVERIFY(view);
    const QUrl closedUrl(
        QStringLiteral("data:text/html,<title>sess-closed</title>x"));
    view->load(closedUrl);
    QTRY_VERIFY_WITH_TIMEOUT(view->url() == closedUrl, 10000);
    tw->closeTab(1);
    QVERIFY(tw->hasRecentlyClosedTabs());

    // sessions.onChanged rode the hub tap into the hosted context.
    QTRY_VERIFY_WITH_TIMEOUT(
        !hostEventsFor(QStringLiteral("sessions.onChanged")).isEmpty(),
        15000);

    // getRecentlyClosed — the closed tab's Session shape.
    reply = invoke(QStringLiteral("getRecentlyClosed"),
                   QJsonObject());
    QVERIFY2(reply.value(QLatin1String("ok")).toBool(),
             qPrintable(reply.value(QLatin1String("error")).toString()));
    QString sessionId;
    {
        const QJsonArray sessions =
            reply.value(QLatin1String("result")).toArray();
        QCOMPARE(sessions.size(), 1);
        const QJsonObject session = sessions.at(0).toObject();
        QVERIFY(session.value(QLatin1String("lastModified")).toDouble()
                > 0);
        const QJsonObject tab =
            session.value(QLatin1String("tab")).toObject();
        QCOMPARE(tab.value(QLatin1String("url")).toString(),
                 closedUrl.toString());
        QCOMPARE(tab.value(QLatin1String("windowId")).toInt(),
                 ExtensionShimBackend::windowIdFor(window));
        QCOMPARE(tab.value(QLatin1String("index")).toInt(), 1);
        QCOMPARE(tab.value(QLatin1String("active")).toBool(), false);
        QCOMPARE(tab.value(QLatin1String("incognito")).toBool(), false);
        sessionId =
            tab.value(QLatin1String("sessionId")).toString();
        QVERIFY(!sessionId.isEmpty());
    }

    // A non-numeric id is an argument error; an unknown one is the
    // typed miss.
    QCOMPARE(invoke(QStringLiteral("restore"), QJsonObject{
                 {QStringLiteral("sessionId"),
                  QStringLiteral("bogus")}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    QCOMPARE(invoke(QStringLiteral("restore"), QJsonObject{
                 {QStringLiteral("sessionId"),
                  QStringLiteral("999999")}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("not-found"));

    // restore(sessionId) — the shared undo path rebuilds the tab;
    // the record is consumed.
    const int beforeRestore = tw->count();
    reply = invoke(QStringLiteral("restore"), QJsonObject{
        {QStringLiteral("sessionId"), sessionId}});
    QVERIFY2(reply.value(QLatin1String("ok")).toBool(),
             qPrintable(reply.value(QLatin1String("error")).toString()));
    {
        const QJsonObject session =
            reply.value(QLatin1String("result")).toObject();
        const QJsonObject tab =
            session.value(QLatin1String("tab")).toObject();
        QCOMPARE(tab.value(QLatin1String("url")).toString(),
                 closedUrl.toString());
        QVERIFY(tab.value(QLatin1String("id")).toInt() > 0);
    }
    QCOMPARE(tw->count(), beforeRestore + 1);
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        for (int i = 0; i < tw->count(); ++i)
            if (tw->webView(i)
                    && tw->webView(i)->url() == closedUrl)
                return true;
        return false;
    }(), 15000);
    // The consumed id answers the typed miss now.
    QCOMPARE(invoke(QStringLiteral("restore"), QJsonObject{
                 {QStringLiteral("sessionId"), sessionId}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("not-found"));
    reply = invoke(QStringLiteral("getRecentlyClosed"),
                   QJsonObject());
    QVERIFY(reply.value(QLatin1String("result")).toArray().isEmpty());

    // Chrome's no-arg form — close the rebuilt tab and restore()
    // takes the newest session.
    int rebuilt = -1;
    for (int i = 0; i < tw->count(); ++i)
        if (tw->webView(i) && tw->webView(i)->url() == closedUrl)
            rebuilt = i;
    QVERIFY(rebuilt >= 0);
    tw->closeTab(rebuilt);
    reply = invoke(QStringLiteral("restore"), QJsonObject());
    QVERIFY2(reply.value(QLatin1String("ok")).toBool(),
             qPrintable(reply.value(QLatin1String("error")).toString()));
    QCOMPARE(reply.value(QLatin1String("result")).toObject()
                 .value(QLatin1String("tab")).toObject()
                 .value(QLatin1String("url")).toString(),
             closedUrl.toString());

    // The manifest gate — a sibling without 'sessions' hears
    // no-permission on every member.
    const QString dirB = QDir::temp().filePath(
        QStringLiteral("tst-extsessb-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dirB));
    {
        QFile f(dirB + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-sess-b\","
                "\"version\":\"1.0\",\"permissions\":[\"storage\"]}");
        f.close();
    }
    ExtensionShimBackend backendB(
        QStringLiteral("tstmockextensionid0043"),
        QStringLiteral("tst-sess-b"), dirB);
    QCOMPARE(backendB.invoke(QStringLiteral("sessions"),
                 QStringLiteral("getDevices"), QJsonObject())
             .value(QLatin1String("error")).toString(),
             QStringLiteral("no-permission"));
    QCOMPARE(backendB.invoke(QStringLiteral("sessions"),
                 QStringLiteral("getRecentlyClosed"), QJsonObject())
             .value(QLatin1String("error")).toString(),
             QStringLiteral("no-permission"));
    QCOMPARE(backendB.invoke(QStringLiteral("sessions"),
                 QStringLiteral("restore"), QJsonObject())
             .value(QLatin1String("error")).toString(),
             QStringLiteral("no-permission"));

    window->deleteLater();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    hub->setTracking(false);
    QDir(dirB).removeRecursively();
}

// MD26 — chrome.tabGroups over the TABGRP machinery: creation rides
// chrome.tabs.group like the real API (the namespace has no create),
// get/query/update/move/ungroup resolve the minted chrome int ids
// onto the strip's per-window groups, and the four on* events fan
// out from the hub's tabGroupsChanged diff tap.
void tst_ExtensionManager::extensionTabGroups()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");

    const QString dir = QDir::temp().filePath(
        QStringLiteral("tst-exttg-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dir));
    {
        QFile f(dir + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-tg\","
                "\"version\":\"1.0\",\"permissions\":[\"tabGroups\"],"
                "\"background\":{\"service_worker\":\"sw.js\"}}");
        f.close();
        QFile w(dir + QLatin1String("/sw.js"));
        QVERIFY(w.open(QIODevice::WriteOnly));
        w.write("self.__tg=[];\n"
                "self.__tgNone=(chrome.tabGroups\n"
                " ? chrome.tabGroups.TAB_GROUP_ID_NONE : null);\n"
                "try{['onCreated','onUpdated','onMoved','onRemoved']\n"
                " .forEach(function(n){chrome.tabGroups[n].addListener(\n"
                "  function(g){self.__tg.push(n+':'+g.id);});});}catch(e){}\n");
        w.close();
    }

    ExtensionManager *manager = ExtensionManager::instance();
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();
    for (const ExtensionManager::ExtensionInfo &info
         : manager->extensions()) {
        if (info.name == QLatin1String("tst-tg"))
            manager->removeExtension(info.id);
    }
    auto loaded = std::make_shared<QString>();
    auto loadErr = std::make_shared<QString>();
    QObject::connect(manager, &ExtensionManager::extensionLoaded,
                     this, [loaded, loadErr](
                         const ExtensionManager::ExtensionInfo &info) {
        if (info.name != QLatin1String("tst-tg"))
            return;
        if (info.error.isEmpty())
            *loaded = info.id;
        else
            *loadErr = info.error;
    });
    manager->grantConsent(dir);
    manager->loadExtension(dir);
    QElapsedTimer timer;
    timer.start();
    while (loaded->isEmpty() && loadErr->isEmpty()
           && timer.elapsed() < 30000)
        QTest::qWait(50);
    const QString extId = *loaded;
    QVERIFY2(!extId.isEmpty(), qPrintable(QStringLiteral(
        "fixture loaded (err=%1)").arg(*loadErr)));
    manager->setExtensionEnabled(extId, true);
    auto unload = qScopeGuard([manager, extId, dir]() {
        manager->setExtensionEnabled(extId, false);
        manager->removeExtension(extId);
        QDir(dir).removeRecursively();
    });
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime->host(extId)
            && runtime->host(extId)->state()
                   == ExtensionBackgroundHost::State::Ready,
        45000);

    ChromeBridge *bridge = runtime->host(extId)->bridge();
    QVERIFY(bridge);
    QSignalSpy hostEvents(bridge, &ChromeBridge::hostEvent);
    auto invoke = [&](const QString &m, const QJsonObject &args) {
        return bridge->invoke(QStringLiteral("tabGroups"), m, args);
    };
    auto invokeTabs = [&](const QString &m, const QJsonObject &args) {
        return bridge->invoke(QStringLiteral("tabs"), m, args);
    };
    auto hostEventsFor = [&](const QString &name) {
        QList<QVariantList> out;
        for (const QVariantList &args : hostEvents)
            if (!args.isEmpty() && args.at(0).toString() == name)
                out.append(args);
        return out;
    };
    auto eval = [](QWebEnginePage *page, const QString &source) {
        auto result = std::make_shared<QVariant>();
        page->runJavaScript(source,
                            [result](const QVariant &v) { *result = v; });
        QElapsedTimer t;
        t.start();
        while (!result->isValid() && t.elapsed() < 10000)
            QTest::qWait(50);
        return *result;
    };

    // The namespace constant landed page-side.
    QCOMPARE(eval(runtime->host(extId)->page(),
                  QStringLiteral("self.__tgNone")).toInt(), -1);

    // Empty-strip answers + the arg refusals needing no group.
    QJsonObject reply = invoke(QStringLiteral("query"), QJsonObject());
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QVERIFY(reply.value(QLatin1String("result")).toArray().isEmpty());
    QCOMPARE(invoke(QStringLiteral("get"), QJsonObject{
                 {QStringLiteral("groupId"),
                  QStringLiteral("bogus")}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    QCOMPARE(invoke(QStringLiteral("get"), QJsonObject{
                 {QStringLiteral("groupId"), 424242.0}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("not-found"));
    QCOMPARE(invoke(QStringLiteral("bogus"), QJsonObject())
             .value(QLatin1String("error")).toString(),
             QStringLiteral("unimplemented"));

    // A real window + three tabs — grouping needs members.
    ExtensionEventHub *hub = ExtensionEventHub::instance();
    hub->setTracking(true);
    hub->syncNow();
    BrowserMainWindow *window = new BrowserMainWindow;
    TabWidget *tw = window->tabWidget();
    QVERIFY(tw && tw->count() >= 1);
    hub->syncNow();   // wires tabGroupsChanged + seeds the baseline
    tw->newTab();
    tw->newTab();
    QVERIFY(tw->count() >= 3);
    WebView *view0 = tw->webView(0);
    WebView *view1 = tw->webView(1);
    WebView *view2 = tw->webView(2);
    QVERIFY(view0 && view1 && view2);
    const int tid1 = ExtensionShimBackend::tabIdFor(view1);
    const int tid2 = ExtensionShimBackend::tabIdFor(view2);

    // UI-originated create first — the tap diff announces it exactly
    // like an API mutation (the hub seeds at wire time, so this is
    // the first observable transition).
    const QString uiGid = tw->createTabGroup(0);
    QVERIFY(!uiGid.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(
        !hostEventsFor(QStringLiteral("tabGroups.onCreated"))
             .isEmpty(),
        15000);
    {
        const QVariantList evt =
            hostEventsFor(QStringLiteral("tabGroups.onCreated"))
                .last();
        const QJsonObject payload = evt.at(1).toJsonObject();
        const QJsonObject group = payload
            .value(QLatin1String("__args")).toArray().at(0).toObject();
        QVERIFY(group.value(QLatin1String("id")).toInt() > 0);
        QCOMPARE(group.value(QLatin1String("windowId")).toInt(),
                 ExtensionShimBackend::windowIdFor(window));
        QCOMPARE(group.value(QLatin1String("collapsed")).toBool(),
                 false);
        tw->ungroupTabs(uiGid);
    }
    QTRY_VERIFY_WITH_TIMEOUT(
        !hostEventsFor(QStringLiteral("tabGroups.onRemoved"))
             .isEmpty(),
        15000);

    // tabs.group is the create path — tabs 1+2 form a new group and
    // the minted chrome id comes back in the reply.
    reply = invokeTabs(QStringLiteral("group"), QJsonObject{
        {QStringLiteral("options"), QJsonObject{
            {QStringLiteral("tabIds"), QJsonArray{tid1, tid2}}}}});
    QVERIFY2(reply.value(QLatin1String("ok")).toBool(),
             qPrintable(reply.value(QLatin1String("error")).toString()));
    const int groupId = reply.value(QLatin1String("result"))
        .toObject().value(QLatin1String("groupId")).toInt();
    QVERIFY(groupId > 0);
    QCOMPARE(tw->tabGroupSize(
                 tw->tabGroupOf(view1)), 2);

    // tabObject.groupId reports the same minted int.
    reply = invokeTabs(QStringLiteral("get"), QJsonObject{
        {QStringLiteral("id"), tid1}});
    QCOMPARE(reply.value(QLatin1String("result")).toObject()
                 .value(QLatin1String("groupId")).toInt(),
             groupId);
    // tabs.onUpdated carried the membership flip as changeInfo.
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        for (const QVariantList &args : hostEvents) {
            if (args.isEmpty()
                    || args.at(0).toString()
                           != QLatin1String("tabs.onUpdated"))
                continue;
            const QJsonArray evtArgs = args.at(1).toJsonObject()
                .value(QLatin1String("__args")).toArray();
            if (evtArgs.size() >= 2
                    && evtArgs.at(1).toObject().contains(
                        QLatin1String("groupId")))
                return true;
        }
        return false;
    }(), 15000);

    // get + query — the live Chrome TabGroup shape.
    reply = invoke(QStringLiteral("get"), QJsonObject{
        {QStringLiteral("groupId"), double(groupId)}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QCOMPARE(reply.value(QLatin1String("result")).toObject()
                 .value(QLatin1String("windowId")).toInt(),
             ExtensionShimBackend::windowIdFor(window));
    reply = invoke(QStringLiteral("query"), QJsonObject{
        {QStringLiteral("query"), QJsonObject{
            {QStringLiteral("windowId"),
             double(ExtensionShimBackend::windowIdFor(window))}}}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QCOMPARE(reply.value(QLatin1String("result")).toArray().size(),
             1);

    // update() — title + color + collapsed all land on the strip
    // record; each mutation answers the live shape.
    reply = invoke(QStringLiteral("update"), QJsonObject{
        {QStringLiteral("groupId"), double(groupId)},
        {QStringLiteral("props"), QJsonObject{
            {QStringLiteral("title"), QStringLiteral("Work")},
            {QStringLiteral("color"), QStringLiteral("blue")}}}});
    QVERIFY2(reply.value(QLatin1String("ok")).toBool(),
             qPrintable(reply.value(QLatin1String("error")).toString()));
    QCOMPARE(reply.value(QLatin1String("result")).toObject()
                 .value(QLatin1String("title")).toString(),
             QStringLiteral("Work"));
    QCOMPARE(reply.value(QLatin1String("result")).toObject()
                 .value(QLatin1String("color")).toString(),
             QStringLiteral("blue"));
    QTRY_VERIFY_WITH_TIMEOUT(
        !hostEventsFor(QStringLiteral("tabGroups.onUpdated"))
             .isEmpty(),
        15000);
    QCOMPARE(invoke(QStringLiteral("update"), QJsonObject{
                 {QStringLiteral("groupId"), double(groupId)},
                 {QStringLiteral("props"), QJsonObject{
                     {QStringLiteral("color"),
                      QStringLiteral("mauve")}}}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    // Filtered query — title substring and color both match.
    reply = invoke(QStringLiteral("query"), QJsonObject{
        {QStringLiteral("query"), QJsonObject{
            {QStringLiteral("title"), QStringLiteral("wor")}}}});
    QCOMPARE(reply.value(QLatin1String("result")).toArray().size(),
             1);
    reply = invoke(QStringLiteral("query"), QJsonObject{
        {QStringLiteral("query"), QJsonObject{
            {QStringLiteral("color"), QStringLiteral("red")}}}});
    QVERIFY(reply.value(QLatin1String("result")).toArray().isEmpty());

    // move() — the run relocates so its first member lands at the
    // index; onMoved reports the group's new shape.
    reply = invoke(QStringLiteral("move"), QJsonObject{
        {QStringLiteral("groupId"), double(groupId)},
        {QStringLiteral("props"), QJsonObject{
            {QStringLiteral("index"), 0.0}}}});
    QVERIFY2(reply.value(QLatin1String("ok")).toBool(),
             qPrintable(reply.value(QLatin1String("error")).toString()));
    const QString gid = tw->tabGroupOf(view1);
    QVERIFY(!gid.isEmpty());
    const QList<int> members = tw->tabGroupMembers(gid);
    QVERIFY(!members.isEmpty());
    QCOMPARE(members.first(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(
        !hostEventsFor(QStringLiteral("tabGroups.onMoved")).isEmpty(),
        15000);
    QCOMPARE(invoke(QStringLiteral("move"), QJsonObject{
                 {QStringLiteral("groupId"), double(groupId)},
                 {QStringLiteral("props"), QJsonObject{
                     {QStringLiteral("index"), -5.0}}}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));

    // ungroup() dissolves the group; the tabs stay.  get() on the
    // stale id answers the typed miss.
    reply = invoke(QStringLiteral("ungroup"), QJsonObject{
        {QStringLiteral("groupIds"), QJsonArray{double(groupId)}}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QVERIFY(tw->tabGroupOf(view1).isEmpty());
    QVERIFY(tw->tabGroupOf(view2).isEmpty());
    QCOMPARE(invoke(QStringLiteral("get"), QJsonObject{
                 {QStringLiteral("groupId"), double(groupId)}})
             .value(QLatin1String("error")).toString(),
             QStringLiteral("not-found"));
    reply = invoke(QStringLiteral("query"), QJsonObject());
    QVERIFY(reply.value(QLatin1String("result")).toArray().isEmpty());

    // The events reached the page-side listeners too — the hosted
    // context's own chrome.tabGroups.* objects fired.
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        const QString seen = eval(
            runtime->host(extId)->page(),
            QStringLiteral("self.__tg.join('|')")).toString();
        return seen.contains(QLatin1String("onCreated:"))
            && seen.contains(QLatin1String("onUpdated:"))
            && seen.contains(QLatin1String("onMoved:"))
            && seen.contains(QLatin1String("onRemoved:"));
    }(), 15000);

    // The manifest gate — a sibling without 'tabGroups' hears
    // no-permission on every member.
    const QString dirB = QDir::temp().filePath(
        QStringLiteral("tst-exttgb-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dirB));
    {
        QFile f(dirB + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-tg-b\","
                "\"version\":\"1.0\",\"permissions\":[\"storage\"]}");
        f.close();
    }
    ExtensionShimBackend backendB(
        QStringLiteral("tstmockextensionid0044"),
        QStringLiteral("tst-tg-b"), dirB);
    for (const QString m : {QStringLiteral("get"),
                            QStringLiteral("query"),
                            QStringLiteral("update"),
                            QStringLiteral("move"),
                            QStringLiteral("ungroup")}) {
        QCOMPARE(backendB.invoke(QStringLiteral("tabGroups"), m,
                                 QJsonObject())
                 .value(QLatin1String("error")).toString(),
                 QStringLiteral("no-permission"));
    }

    window->deleteLater();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    hub->setTracking(false);
    QDir(dirB).removeRecursively();
}

// MD27 — chrome.topSites: get() answers the visit-count-ranked
// MostVisitedURL list off the RC04 hosts rollup (the C++ aggregate
// mirrors it when rustcore is off); 'topSites' gates the namespace
// like Chrome, and get() is the only published member.
void tst_ExtensionManager::extensionTopSites()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");

    // Same ambient-persona binding the MD22 leg uses — the backend's
    // HistoryManager::instance() resolves exactly this store, the
    // ambient id restores on exit, the persona dir goes with it.
    const QString prevId = BrowserPaths::activeProfileId();
    const QString profileId = QStringLiteral("tstexttop%1")
            .arg(QCoreApplication::applicationPid());
    BrowserPaths::setActiveProfileId(profileId);
    HistoryManager *manager = HistoryManager::instance();
    QVERIFY(manager);
    QCOMPARE(manager->profileId(), profileId);
    auto unbind = qScopeGuard([prevId, profileId]() {
        HistoryManager::releaseFor(profileId);
        BrowserPaths::setActiveProfileId(prevId);
        QDir(BrowserPaths::profileDataDir(profileId))
            .removeRecursively();
    });
    manager->clear();

    // Two hosts where the heavier one is NOT the newest — the rank
    // must follow visit count, not recency, and the representative
    // entry is the host's newest visit.
    const QDateTime now = QDateTime::currentDateTime();
    QList<HistoryEntry> seed;
    seed.append(HistoryEntry(
        QStringLiteral("https://top.example/first"),
        now.addSecs(-30), QStringLiteral("Top first")));
    seed.append(HistoryEntry(
        QStringLiteral("https://top.example/latest"),
        now.addSecs(-10), QStringLiteral("Top latest")));
    seed.append(HistoryEntry(
        QStringLiteral("https://other.example/"),
        now, QStringLiteral("Other")));
    manager->setHistory(seed);
    QCOMPARE(manager->history().size(), 3);

    const QString dir = QDir::temp().filePath(
        QStringLiteral("tst-exttop-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dir));
    {
        QFile f(dir + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-top\","
                "\"version\":\"1.0\",\"permissions\":[\"topSites\"]}");
        f.close();
    }
    ExtensionShimBackend backend(
        QStringLiteral("tstmockextensionid0045"),
        QStringLiteral("tst-top"), dir);

    QJsonObject reply = backend.invoke(
        QStringLiteral("topSites"), QStringLiteral("get"),
        QJsonObject());
    QVERIFY2(reply.value(QLatin1String("ok")).toBool(),
             qPrintable(reply.value(QLatin1String("error")).toString()));
    const QJsonArray sites =
        reply.value(QLatin1String("result")).toArray();
    QCOMPARE(sites.size(), 2);
    // The visited host leads — two visits outrank the single newer
    // one, and the newest visit supplies the MostVisitedURL.
    QCOMPARE(sites.at(0).toObject().value(QLatin1String("url"))
                 .toString(),
             QStringLiteral("https://top.example/latest"));
    QCOMPARE(sites.at(0).toObject().value(QLatin1String("title"))
                 .toString(),
             QStringLiteral("Top latest"));
    QCOMPARE(sites.at(1).toObject().value(QLatin1String("url"))
                 .toString(),
             QStringLiteral("https://other.example/"));

    // get() is the only published member — a mistyped bridge call is
    // the typed unreachable-member verdict.
    QCOMPARE(backend.invoke(QStringLiteral("topSites"),
                            QStringLiteral("peek"), QJsonObject())
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("unimplemented"));

    // The manifest gate is per-context: a sibling backend without
    // 'topSites' hears no-permission.
    const QString dirB = QDir::temp().filePath(
        QStringLiteral("tst-exttopb-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dirB));
    {
        QFile f(dirB + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-top-b\","
                "\"version\":\"1.0\",\"permissions\":[\"storage\"]}");
        f.close();
    }
    ExtensionShimBackend backendB(
        QStringLiteral("tstmockextensionid0046"),
        QStringLiteral("tst-top-b"), dirB);
    QCOMPARE(backendB.invoke(QStringLiteral("topSites"),
                             QStringLiteral("get"), QJsonObject())
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("no-permission"));

    QDir(dirB).removeRecursively();
}

// MD28 — chrome.browsingData over the CLR01 clear plan: remove() and
// the five served remove* members (history/cookies/downloads/cache/
// formData) map Chrome's DataTypeSet onto ClearPrivateData category
// bits, 'browsingData' gates the namespace, and the nine unserved
// dataTypes + unservable RemovalOptions scoping keep typed verdicts
// — reported inside the removal result, never silently skipped.
void tst_ExtensionManager::extensionBrowsingData()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");

    // Same ambient-persona binding the MD27 leg uses — the backend's
    // HistoryManager/AutoFillManager resolve exactly these stores.
    const QString prevId = BrowserPaths::activeProfileId();
    const QString profileId = QStringLiteral("tstextbd%1")
            .arg(QCoreApplication::applicationPid());
    BrowserPaths::setActiveProfileId(profileId);
    HistoryManager *manager = HistoryManager::instance();
    QVERIFY(manager);
    QCOMPARE(manager->profileId(), profileId);
    auto unbind = qScopeGuard([prevId, profileId]() {
        HistoryManager::releaseFor(profileId);
        BrowserPaths::setActiveProfileId(prevId);
        QDir(BrowserPaths::profileDataDir(profileId))
            .removeRecursively();
    });
    manager->clear();

    // A two-hour-old row and a fresh one — the windowed remove must
    // take only the in-window row.
    const QDateTime now = QDateTime::currentDateTime();
    QList<HistoryEntry> seed;
    seed.append(HistoryEntry(
        QStringLiteral("https://old.example/"),
        now.addSecs(-7200), QStringLiteral("Old")));
    seed.append(HistoryEntry(
        QStringLiteral("https://new.example/"),
        now, QStringLiteral("New")));
    manager->setHistory(seed);
    QCOMPARE(manager->history().size(), 2);

    const QString dir = QDir::temp().filePath(
        QStringLiteral("tst-extbd-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dir));
    {
        QFile f(dir + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-bd\","
                "\"version\":\"1.0\",\"permissions\":[\"browsingData\"]}");
        f.close();
    }
    ExtensionShimBackend backend(
        QStringLiteral("tstmockextensionid0047"),
        QStringLiteral("tst-bd"), dir);

    // settings() answers Chrome's {options, dataToRemove,
    // dataRemovalPermitted} — served types permitted, the other nine
    // honestly not.
    QJsonObject reply = backend.invoke(
        QStringLiteral("browsingData"), QStringLiteral("settings"),
        QJsonObject());
    QVERIFY2(reply.value(QLatin1String("ok")).toBool(),
             qPrintable(reply.value(QLatin1String("error")).toString()));
    const QJsonObject settings =
        reply.value(QLatin1String("result")).toObject();
    const QJsonObject permitted = settings
        .value(QLatin1String("dataRemovalPermitted")).toObject();
    QCOMPARE(permitted.value(QLatin1String("history")).toBool(), true);
    QCOMPARE(permitted.value(QLatin1String("cookies")).toBool(), true);
    QCOMPARE(permitted.value(QLatin1String("downloads")).toBool(), true);
    QCOMPARE(permitted.value(QLatin1String("cache")).toBool(), true);
    QCOMPARE(permitted.value(QLatin1String("formData")).toBool(), true);
    QCOMPARE(permitted.value(QLatin1String("passwords")).toBool(), false);
    QCOMPARE(permitted.value(QLatin1String("localStorage")).toBool(), false);
    QCOMPARE(settings.value(QLatin1String("options")).toObject()
                 .value(QLatin1String("originTypes")).toObject()
                 .value(QLatin1String("unprotectedWeb")).toBool(),
             true);

    // removeHistory with a 'since' stamp 60s back: the plan carries
    // it as an exact secs:<n> window — the fresh row goes, the
    // two-hour-old one survives, and the widening past the window on
    // the unranged companion store is reported under 'degraded'.
    const double sinceMs =
        double(now.toMSecsSinceEpoch() - 60 * 1000);
    reply = backend.invoke(
        QStringLiteral("browsingData"), QStringLiteral("removeHistory"),
        QJsonObject{{QStringLiteral("options"),
                     QJsonObject{{QStringLiteral("since"), sinceMs}}}});
    QVERIFY2(reply.value(QLatin1String("ok")).toBool(),
             qPrintable(reply.value(QLatin1String("error")).toString()));
    const QJsonObject result =
        reply.value(QLatin1String("result")).toObject();
    QVERIFY(result.value(QLatin1String("removed")).toArray()
                .contains(QStringLiteral("history")));
    QVERIFY(result.value(QLatin1String("degraded")).toArray()
                .contains(QStringLiteral("history")));
    QVERIFY(manager->historyContains(
                QStringLiteral("https://old.example/")));
    QVERIFY(!manager->historyContains(
                QStringLiteral("https://new.example/")));

    // remove() with a mixed DataTypeSet: the served member executes,
    // the unserved one reports inside the removal set — not skipped.
    manager->setHistory(seed);
    reply = backend.invoke(
        QStringLiteral("browsingData"), QStringLiteral("remove"),
        QJsonObject{{QStringLiteral("options"), QJsonObject()},
                    {QStringLiteral("dataToRemove"),
                     QJsonObject{{QStringLiteral("history"), true},
                                 {QStringLiteral("passwords"), true}}}});
    QVERIFY2(reply.value(QLatin1String("ok")).toBool(),
             qPrintable(reply.value(QLatin1String("error")).toString()));
    const QJsonObject mixed =
        reply.value(QLatin1String("result")).toObject();
    QVERIFY(mixed.value(QLatin1String("removed")).toArray()
                .contains(QStringLiteral("history")));
    QVERIFY(mixed.value(QLatin1String("unsupported")).toArray()
                .contains(QStringLiteral("passwords")));
    QVERIFY(manager->history().isEmpty());

    // A call whose WHOLE requested set is unservable answers the
    // typed verdict — nothing was cleared.
    QCOMPARE(backend.invoke(
                 QStringLiteral("browsingData"),
                 QStringLiteral("remove"),
                 QJsonObject{{QStringLiteral("dataToRemove"),
                              QJsonObject{
                                  {QStringLiteral("passwords"), true},
                                  {QStringLiteral("localStorage"), true}}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("unsupported"));
    // A dedicated removeX member on an unserved store answers the
    // same typed verdict.
    QCOMPARE(backend.invoke(
                 QStringLiteral("browsingData"),
                 QStringLiteral("removePasswords"), QJsonObject())
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("unsupported"));
    // Per-origin scoping has no whole-store equivalent — refused,
    // not widened.
    QCOMPARE(backend.invoke(
                 QStringLiteral("browsingData"),
                 QStringLiteral("removeHistory"),
                 QJsonObject{{QStringLiteral("options"),
                              QJsonObject{
                                  {QStringLiteral("origins"),
                                   QJsonArray{
                                       QStringLiteral("https://a.example")}}}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("unsupported"));
    // Scoping the regular web out leaves nothing we serve.
    QCOMPARE(backend.invoke(
                 QStringLiteral("browsingData"),
                 QStringLiteral("removeHistory"),
                 QJsonObject{{QStringLiteral("options"),
                              QJsonObject{
                                  {QStringLiteral("originTypes"),
                                   QJsonObject{
                                       {QStringLiteral("unprotectedWeb"),
                                        false}}}}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("unsupported"));
    // Missing/mistyped args are the argument verdicts.
    QCOMPARE(backend.invoke(
                 QStringLiteral("browsingData"),
                 QStringLiteral("remove"), QJsonObject())
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    QCOMPARE(backend.invoke(
                 QStringLiteral("browsingData"),
                 QStringLiteral("removeHistory"),
                 QJsonObject{{QStringLiteral("options"),
                              QJsonObject{
                                  {QStringLiteral("since"),
                                   QStringLiteral("abc")}}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    // A mistyped bridge call is the typed unreachable-member verdict.
    QCOMPARE(backend.invoke(
                 QStringLiteral("browsingData"),
                 QStringLiteral("purge"), QJsonObject())
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("unimplemented"));

    // The manifest gate is per-context: a sibling backend without
    // 'browsingData' hears no-permission on every member.
    const QString dirB = QDir::temp().filePath(
        QStringLiteral("tst-extbdb-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dirB));
    {
        QFile f(dirB + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-bd-b\","
                "\"version\":\"1.0\",\"permissions\":[\"storage\"]}");
        f.close();
    }
    ExtensionShimBackend backendB(
        QStringLiteral("tstmockextensionid0048"),
        QStringLiteral("tst-bd-b"), dirB);
    for (const QString m : {QStringLiteral("settings"),
                            QStringLiteral("remove"),
                            QStringLiteral("removeHistory"),
                            QStringLiteral("removeCookies")}) {
        QCOMPARE(backendB.invoke(QStringLiteral("browsingData"), m,
                                 QJsonObject())
                     .value(QLatin1String("error")).toString(),
                 QStringLiteral("no-permission"));
    }

    QDir(dirB).removeRecursively();
}

// MD29 — chrome.dns over QHostInfo: resolve() is the only member,
// 'dns' gates the namespace like Chrome's declared grant, and the
// lookup is deferred — the invoke answers a ticket and the verdict
// lands on the '__arktos.dnsResolved' push as {addresses:[...]} or
// a typed resolve-failed.
void tst_ExtensionManager::extensionDns()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");

    const QString dir = QDir::temp().filePath(
        QStringLiteral("tst-extdns-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dir));
    {
        QFile f(dir + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-dns\","
                "\"version\":\"1.0\",\"permissions\":[\"dns\"]}");
        f.close();
    }
    ExtensionShimBackend backend(
        QStringLiteral("tstmockextensionid0049"),
        QStringLiteral("tst-dns"), dir);
    QSignalSpy pushSpy(&backend, &ExtensionShimBackend::pushEvent);
    auto dnsPushes = [&pushSpy]() {
        QList<QJsonObject> out;
        for (const QVariantList &args : pushSpy) {
            if (args.size() >= 2
                    && args.at(0).toString()
                           == QLatin1String("__arktos.dnsResolved"))
                out.append(args.at(1).toJsonObject());
        }
        return out;
    };

    // Argument verdicts before any ticket is minted.
    QCOMPARE(backend.invoke(QStringLiteral("dns"),
                            QStringLiteral("resolve"), QJsonObject())
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    QCOMPARE(backend.invoke(
                 QStringLiteral("dns"), QStringLiteral("resolve"),
                 QJsonObject{{QStringLiteral("hostname"), 42.0}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    QCOMPARE(backend.invoke(
                 QStringLiteral("dns"), QStringLiteral("resolve"),
                 QJsonObject{{QStringLiteral("hostname"),
                              QStringLiteral("")}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    QCOMPARE(backend.invoke(
                 QStringLiteral("dns"), QStringLiteral("resolve"),
                 QJsonObject{{QStringLiteral("hostname"),
                              QString(254, QLatin1Char('a'))}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    // resolve is the only published member — anything else is the
    // typed unreachable-member verdict.
    QCOMPARE(backend.invoke(QStringLiteral("dns"),
                            QStringLiteral("flush"), QJsonObject())
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("unimplemented"));

    // The happy path: 'localhost' resolves off /etc/hosts — no
    // network needed — and the ticket lands a non-empty addresses
    // list carrying a loopback address.
    QJsonObject reply = backend.invoke(
        QStringLiteral("dns"), QStringLiteral("resolve"),
        QJsonObject{{QStringLiteral("hostname"),
                     QStringLiteral("localhost")}});
    QVERIFY2(reply.value(QLatin1String("ok")).toBool(),
             qPrintable(reply.value(QLatin1String("error")).toString()));
    const int ticket = reply.value(QLatin1String("result")).toObject()
        .value(QLatin1String("ticket")).toInt();
    QVERIFY(ticket > 0);
    QTRY_VERIFY_WITH_TIMEOUT(!dnsPushes().isEmpty(), 30000);
    {
        const QJsonObject body = dnsPushes().constLast();
        QCOMPARE(body.value(QLatin1String("ticket")).toInt(), ticket);
        QVERIFY(!body.contains(QLatin1String("error")));
        const QJsonArray addresses =
            body.value(QLatin1String("addresses")).toArray();
        QVERIFY(!addresses.isEmpty());
        bool loopback = false;
        for (const QJsonValue &v : addresses) {
            const QHostAddress address(v.toString());
            loopback = loopback || address.isLoopback();
        }
        QVERIFY(loopback);
    }

    // The failure path: a name getaddrinfo cannot accept produces a
    // typed resolve-failed on the same push channel — never a hang.
    reply = backend.invoke(
        QStringLiteral("dns"), QStringLiteral("resolve"),
        QJsonObject{{QStringLiteral("hostname"),
                     QStringLiteral("not a host")}});
    QVERIFY2(reply.value(QLatin1String("ok")).toBool(),
             qPrintable(reply.value(QLatin1String("error")).toString()));
    const int badTicket = reply.value(QLatin1String("result"))
        .toObject().value(QLatin1String("ticket")).toInt();
    QVERIFY(badTicket > 0);
    QVERIFY(badTicket != ticket);
    QTRY_VERIFY_WITH_TIMEOUT(dnsPushes().size() >= 2, 30000);
    {
        const QJsonObject body = dnsPushes().constLast();
        QCOMPARE(body.value(QLatin1String("ticket")).toInt(),
                 badTicket);
        QCOMPARE(body.value(QLatin1String("error")).toString(),
                 QStringLiteral("resolve-failed"));
    }

    // The manifest gate is per-context: a sibling backend without
    // 'dns' hears no-permission.
    const QString dirB = QDir::temp().filePath(
        QStringLiteral("tst-extdnsb-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dirB));
    {
        QFile f(dirB + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-dns-b\","
                "\"version\":\"1.0\",\"permissions\":[\"storage\"]}");
        f.close();
    }
    ExtensionShimBackend backendB(
        QStringLiteral("tstmockextensionid0050"),
        QStringLiteral("tst-dns-b"), dirB);
    QCOMPARE(backendB.invoke(
                 QStringLiteral("dns"), QStringLiteral("resolve"),
                 QJsonObject{{QStringLiteral("hostname"),
                              QStringLiteral("localhost")}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("no-permission"));

    QDir(dirB).removeRecursively();
    QDir(dir).removeRecursively();
}

// MD30 — chrome.system over measured machine facts: each
// sub-namespace gates on its own 'system.<sub>' declared grant
// (checked BEFORE the surface so a declared-but-unserved sub hears
// 'unsupported', not 'no-permission'); memory/cpu/display/storage
// getInfo answer real values plus storage.getAvailableCapacity.
void tst_ExtensionManager::extensionSystem()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");

    const QString dir = QDir::temp().filePath(
        QStringLiteral("tst-extsys-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dir));
    {
        QFile f(dir + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-sys\","
                "\"version\":\"1.0\",\"permissions\":[\"system.memory\","
                "\"system.cpu\",\"system.display\",\"system.storage\","
                "\"system.network\"]}");
        f.close();
    }
    ExtensionShimBackend backend(
        QStringLiteral("tstmockextensionid0051"),
        QStringLiteral("tst-sys"), dir);
    const auto invoke = [&backend](const QString &method,
                                   const QJsonObject &args
                                       = QJsonObject()) {
        return backend.invoke(QStringLiteral("system"), method, args);
    };

    // --- memory.getInfo: a measured capacity ----------------------
    QJsonObject reply = invoke(QStringLiteral("memory.getInfo"));
    QVERIFY2(reply.value(QLatin1String("ok")).toBool(),
             qPrintable(reply.value(QLatin1String("error")).toString()));
    const QJsonObject memory =
        reply.value(QLatin1String("result")).toObject();
    QVERIFY(memory.value(QLatin1String("capacity")).toDouble() > 0);
    // MemAvailable exists on every Linux — where it was measured it
    // can never exceed the total.
    if (memory.contains(QLatin1String("availableCapacity"))) {
        QVERIFY(memory.value(QLatin1String("availableCapacity"))
                    .toDouble() >= 0);
        QVERIFY(memory.value(QLatin1String("availableCapacity"))
                    .toDouble()
                <= memory.value(QLatin1String("capacity")).toDouble());
    }

    // --- cpu.getInfo: mapped arch + real per-core rows ------------
    reply = invoke(QStringLiteral("cpu.getInfo"));
    QVERIFY2(reply.value(QLatin1String("ok")).toBool(),
             qPrintable(reply.value(QLatin1String("error")).toString()));
    const QJsonObject cpu =
        reply.value(QLatin1String("result")).toObject();
    QVERIFY(!cpu.value(QLatin1String("archName")).toString()
                 .isEmpty());
    QVERIFY(cpu.value(QLatin1String("numOfProcessors")).toInt() >= 1);
#if defined(Q_OS_LINUX)
    // /proc/stat is the per-core source — cumulative seconds per
    // usage field, one row per core.
    const QJsonArray processors =
        cpu.value(QLatin1String("processors")).toArray();
    QVERIFY(processors.size() >= 1);
    for (const QJsonValue &v : processors) {
        const QJsonObject usage =
            v.toObject().value(QLatin1String("usage")).toObject();
        QVERIFY(usage.value(QLatin1String("user")).toDouble() >= 0);
        QVERIFY(usage.value(QLatin1String("kernel")).toDouble() >= 0);
        QVERIFY(usage.value(QLatin1String("idle")).toDouble() >= 0);
        QVERIFY(usage.value(QLatin1String("total")).toDouble() >= 0);
    }
#endif

    // --- display.getInfo: the live screen list --------------------
    reply = invoke(QStringLiteral("display.getInfo"));
    QVERIFY2(reply.value(QLatin1String("ok")).toBool(),
             qPrintable(reply.value(QLatin1String("error")).toString()));
    const QJsonArray displays =
        reply.value(QLatin1String("result")).toArray();
    // The offscreen platform still reports its one screen.
    QVERIFY(!displays.isEmpty());
    bool sawPrimary = false;
    for (const QJsonValue &v : displays) {
        const QJsonObject display = v.toObject();
        QVERIFY(!display.value(QLatin1String("id")).toString()
                     .isEmpty());
        QVERIFY(display.value(QLatin1String("isEnabled")).toBool());
        const QJsonObject bounds =
            display.value(QLatin1String("bounds")).toObject();
        QVERIFY(bounds.value(QLatin1String("width")).toInt() > 0);
        QVERIFY(bounds.value(QLatin1String("height")).toInt() > 0);
        QVERIFY(display.contains(QLatin1String("workArea")));
        const int rotation =
            display.value(QLatin1String("rotation")).toInt();
        QVERIFY(rotation == 0 || rotation == 90 || rotation == 180
                || rotation == 270);
        sawPrimary = sawPrimary
            || display.value(QLatin1String("isPrimary")).toBool();
    }
    QVERIFY(sawPrimary);
    // A flags object rides through; a mistyped one is the argument
    // verdict.
    QVERIFY(invoke(QStringLiteral("display.getInfo"),
                   QJsonObject{{QStringLiteral("flags"), QJsonObject()}})
                .value(QLatin1String("ok")).toBool());
    QCOMPARE(invoke(QStringLiteral("display.getInfo"),
                    QJsonObject{{QStringLiteral("flags"), 7}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));

    // --- storage.getInfo + getAvailableCapacity -------------------
    reply = invoke(QStringLiteral("storage.getInfo"));
    QVERIFY2(reply.value(QLatin1String("ok")).toBool(),
             qPrintable(reply.value(QLatin1String("error")).toString()));
    const QJsonArray units =
        reply.value(QLatin1String("result")).toArray();
    QVERIFY(!units.isEmpty());
    const QJsonObject unit = units.at(0).toObject();
    const QString unitId =
        unit.value(QLatin1String("id")).toString();
    QVERIFY(!unitId.isEmpty());
    QCOMPARE(unit.value(QLatin1String("type")).toString(),
             QStringLiteral("fixed"));
    QVERIFY(unit.value(QLatin1String("capacity")).toDouble() > 0);

    reply = invoke(QStringLiteral("storage.getAvailableCapacity"),
                   QJsonObject{{QStringLiteral("id"), unitId}});
    QVERIFY2(reply.value(QLatin1String("ok")).toBool(),
             qPrintable(reply.value(QLatin1String("error")).toString()));
    const QJsonObject available =
        reply.value(QLatin1String("result")).toObject();
    QCOMPARE(available.value(QLatin1String("id")).toString(), unitId);
    QVERIFY(available.value(QLatin1String("availableCapacity"))
                .toDouble() >= 0);
    QVERIFY(available.value(QLatin1String("availableCapacity"))
                .toDouble()
            <= unit.value(QLatin1String("capacity")).toDouble());
    // An unknown unit id is not-found; a missing one is the
    // argument verdict.
    QCOMPARE(invoke(QStringLiteral("storage.getAvailableCapacity"),
                    QJsonObject{{QStringLiteral("id"),
                                 QStringLiteral("/dev/nope0")}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("not-found"));
    QCOMPARE(invoke(QStringLiteral("storage.getAvailableCapacity"))
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));

    // --- typed verdicts --------------------------------------------
    // ChromeOS mutators + ejectDevice inside a served sub.
    QCOMPARE(invoke(QStringLiteral("display.setMirrorMode"))
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("unsupported"));
    QCOMPARE(invoke(QStringLiteral("storage.ejectDevice"),
                    QJsonObject{{QStringLiteral("id"), unitId}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("unsupported"));
    QCOMPARE(invoke(QStringLiteral("cpu.freeze"))
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("unsupported"));
    // Declared 'system.network' — the grant was honored, the
    // surface is missing: 'unsupported', not 'no-permission'.
    QCOMPARE(invoke(QStringLiteral("network.getNetworkInterfaces"))
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("unsupported"));
    // An undeclared sub stays no-permission.
    QCOMPARE(invoke(QStringLiteral("powerSource.getInfo"))
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("no-permission"));
    // A flat member name is the unreachable-member verdict.
    QCOMPARE(invoke(QStringLiteral("getInfo"))
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("unimplemented"));

    // The manifest gate is per-context: a sibling backend without
    // any 'system.*' grant hears no-permission on every sub.
    const QString dirB = QDir::temp().filePath(
        QStringLiteral("tst-extsysb-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dirB));
    {
        QFile f(dirB + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-sys-b\","
                "\"version\":\"1.0\",\"permissions\":[\"storage\"]}");
        f.close();
    }
    ExtensionShimBackend backendB(
        QStringLiteral("tstmockextensionid0052"),
        QStringLiteral("tst-sys-b"), dirB);
    for (const QString m : {QStringLiteral("memory.getInfo"),
                            QStringLiteral("cpu.getInfo"),
                            QStringLiteral("display.getInfo"),
                            QStringLiteral("storage.getInfo"),
                            QStringLiteral("network.getNetworkInterfaces")}) {
        QCOMPARE(backendB.invoke(QStringLiteral("system"), m,
                                 QJsonObject())
                     .value(QLatin1String("error")).toString(),
                 QStringLiteral("no-permission"));
    }

    QDir(dirB).removeRecursively();
    QDir(dir).removeRecursively();
}

// MD31 — chrome.privacy over the browser's real settings.  The eight
// mapped ChromeSetting members roundtrip the persona's persisted
// toggles with live re-application; control layering, clear-restore,
// scope refusals and the unserved-vocabulary verdicts are asserted
// the same way.
void tst_ExtensionManager::extensionPrivacy()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");

    const QString dir = QDir::temp().filePath(
        QStringLiteral("tst-extpriv-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dir));
    {
        QFile f(dir + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-priv\","
                "\"version\":\"1.0\",\"permissions\":[\"privacy\"]}");
        f.close();
    }
    ExtensionShimBackend backend(
        QStringLiteral("tstmockextensionid0053"),
        QStringLiteral("tst-priv"), dir);
    const auto invoke = [&backend](const QString &method,
                                   const QJsonObject &args
                                       = QJsonObject()) {
        return backend.invoke(QStringLiteral("privacy"), method, args);
    };
    QSignalSpy pushSpy(&backend, &ExtensionShimBackend::pushEvent);

    const QString profileId = BrowserPaths::activeProfileId();

    // The leg writes real persona state — snapshot every key it can
    // touch so the restore below leaves the profile as found.
    ProfileSettings store(profileId);
    const bool origBlock3p = store.value(
        QLatin1String("cookies/blockThirdPartyCookies"), true).toBool();
    const bool origBlockPings = store.value(
        QLatin1String("privacy/blockPings"), true).toBool();
    const QVariant origReferer =
        store.value(QLatin1String("privacy/refererPolicy"));
    const bool origTrimReferer = store.value(
        QLatin1String("privacy/trimReferer"), true).toBool();
    const bool origGpc = store.value(
        QLatin1String("privacy/gpcSignal"), true).toBool();
    const bool origBlockPrefetch = store.value(
        QLatin1String("privacy/blockPrefetch"), true).toBool();
    const bool origDnsPrefetch = store.value(
        QLatin1String("privacy/dnsPrefetch"), false).toBool();
    const bool origWebrtc = store.value(
        QLatin1String("privacy/webrtcIpProtection"), true).toBool();
    const bool origPasswordForms = store.value(
        QLatin1String("autofill/passwordForms"), true).toBool();
    const bool origSuggest = ToolbarSearch::openSearchManager()
        ->suggestionsInAddressField();

    const auto get = [&invoke](const QString &path) {
        const QJsonObject reply = invoke(path + QLatin1String(".get"));
        return reply.value(QLatin1String("result")).toObject();
    };
    const auto setBool = [&invoke](const QString &path, bool value) {
        return invoke(path + QLatin1String(".set"),
                      QJsonObject{{QStringLiteral("details"),
                                   QJsonObject{{QStringLiteral("value"),
                                                value}}}});
    };

    // --- websites.thirdPartyCookiesAllowed — get/set/clear over the
    // real cookie policy -------------------------------------------
    const bool orig3pAllowed = !origBlock3p;
    QCOMPARE(get(QStringLiteral("websites.thirdPartyCookiesAllowed"))
                 .value(QLatin1String("value")).toBool(),
             orig3pAllowed);
    QCOMPARE(get(QStringLiteral("websites.thirdPartyCookiesAllowed"))
                 .value(QLatin1String("levelOfControl")).toString(),
             QStringLiteral("controllable_by_this_extension"));

    QVERIFY2(setBool(QStringLiteral(
                         "websites.thirdPartyCookiesAllowed"), true)
                 .value(QLatin1String("ok")).toBool(),
             qPrintable(setBool(
                 QStringLiteral("websites.thirdPartyCookiesAllowed"),
                 true).value(QLatin1String("error")).toString()));
    // The write landed the persisted key AND the live jar policy —
    // Chrome's set() semantics are real, not recorded-only.
    QCOMPARE(ProfileSettings(profileId).value(
                 QLatin1String("cookies/blockThirdPartyCookies"),
                 true).toBool(), false);
    QCOMPARE(CookieJar::instanceFor(profileId)
                 ->blockThirdPartyCookies(), false);
    QCOMPARE(get(QStringLiteral("websites.thirdPartyCookiesAllowed"))
                 .value(QLatin1String("value")).toBool(), true);
    QCOMPARE(get(QStringLiteral("websites.thirdPartyCookiesAllowed"))
                 .value(QLatin1String("levelOfControl")).toString(),
             QStringLiteral("controlled_by_this_extension"));
    // The API-driven mutation fanned the ChromeSetting's onChange
    // out — the bare backend hears it on pushEvent with the actor's
    // own control level.
    bool saw3pChange = false;
    for (const QList<QVariant> &call : pushSpy) {
        if (call.value(0).toString() != QLatin1String(
                "privacy.websites.thirdPartyCookiesAllowed.onChange"))
            continue;
        const QJsonObject details = call.value(1).toJsonObject()
            .value(QLatin1String("__args")).toArray().first()
                .toObject();
        QCOMPARE(details.value(QLatin1String("value")).toBool(), true);
        QCOMPARE(details.value(QLatin1String("levelOfControl"))
                     .toString(),
                 QStringLiteral("controlled_by_this_extension"));
        saw3pChange = true;
    }
    QVERIFY(saw3pChange);

    // --- websites.hyperlinkAuditingEnabled <- blockPings ----------
    QCOMPARE(get(QStringLiteral("websites.hyperlinkAuditingEnabled"))
                 .value(QLatin1String("value")).toBool(),
             !origBlockPings);
    QVERIFY(setBool(QStringLiteral(
                        "websites.hyperlinkAuditingEnabled"), true)
                .value(QLatin1String("ok")).toBool());
    QCOMPARE(ProfileSettings(profileId).value(
                 QLatin1String("privacy/blockPings"), true).toBool(),
             false);
    QCOMPARE(get(QStringLiteral("websites.hyperlinkAuditingEnabled"))
                 .value(QLatin1String("value")).toBool(), true);

    // --- websites.referrersEnabled <- privacy/refererPolicy -------
    QVERIFY(setBool(QStringLiteral("websites.referrersEnabled"), false)
                .value(QLatin1String("ok")).toBool());
    QCOMPARE(ProfileSettings(profileId).value(
                 QLatin1String("privacy/refererPolicy")).toInt(), 3);
    QCOMPARE(get(QStringLiteral("websites.referrersEnabled"))
                 .value(QLatin1String("value")).toBool(), false);
    // The controller's clear() restores the pre-extension value —
    // Chrome's restore-default.
    QVERIFY(invoke(QStringLiteral("websites.referrersEnabled.clear"))
                .value(QLatin1String("ok")).toBool());
    const int restoredReferer = origReferer.isValid()
        ? origReferer.toInt() : (origTrimReferer ? 1 : 0);
    QCOMPARE(ProfileSettings(profileId).value(
                 QLatin1String("privacy/refererPolicy")).toInt(),
             restoredReferer);
    QCOMPARE(get(QStringLiteral("websites.referrersEnabled"))
                 .value(QLatin1String("levelOfControl")).toString(),
             QStringLiteral("controllable_by_this_extension"));

    // --- websites.doNotTrackEnabled <- privacy/gpcSignal ----------
    QCOMPARE(get(QStringLiteral("websites.doNotTrackEnabled"))
                 .value(QLatin1String("value")).toBool(), origGpc);
    QVERIFY(setBool(QStringLiteral("websites.doNotTrackEnabled"),
                    false).value(QLatin1String("ok")).toBool());
    QCOMPARE(ProfileSettings(profileId).value(
                 QLatin1String("privacy/gpcSignal"), true).toBool(),
             false);

    // --- network.networkPredictionEnabled <- blockPrefetch --------
    QCOMPARE(get(QStringLiteral("network.networkPredictionEnabled"))
                 .value(QLatin1String("value")).toBool(),
             !origBlockPrefetch);
    QVERIFY(setBool(QStringLiteral("network.networkPredictionEnabled"),
                    true).value(QLatin1String("ok")).toBool());
    QCOMPARE(ProfileSettings(profileId).value(
                 QLatin1String("privacy/blockPrefetch"), true).toBool(),
             false);
    // Prediction-off also drops the opt-in DNS prefetch — Chrome's
    // semantic folds every speculative channel.
    ProfileSettings(profileId).setValue(
        QLatin1String("privacy/dnsPrefetch"), true);
    QVERIFY(setBool(QStringLiteral("network.networkPredictionEnabled"),
                    false).value(QLatin1String("ok")).toBool());
    QCOMPARE(ProfileSettings(profileId).value(
                 QLatin1String("privacy/blockPrefetch"), true).toBool(),
             true);
    QCOMPARE(ProfileSettings(profileId).value(
                 QLatin1String("privacy/dnsPrefetch"), true).toBool(),
             false);

    // --- network.webRTCIPHandlingPolicy <- webrtcIpProtection -----
    QCOMPARE(get(QStringLiteral("network.webRTCIPHandlingPolicy"))
                 .value(QLatin1String("value")).toString(),
             origWebrtc ? QStringLiteral("disable_non_proxied_udp")
                        : QStringLiteral("default"));
    QVERIFY(invoke(QStringLiteral("network.webRTCIPHandlingPolicy.set"),
                   QJsonObject{{QStringLiteral("details"),
                                QJsonObject{{QStringLiteral("value"),
                                             QStringLiteral("default")}}}})
                .value(QLatin1String("ok")).toBool());
    QCOMPARE(ProfileSettings(profileId).value(
                 QLatin1String("privacy/webrtcIpProtection"),
                 true).toBool(), false);
    QCOMPARE(get(QStringLiteral("network.webRTCIPHandlingPolicy"))
                 .value(QLatin1String("value")).toString(),
             QStringLiteral("default"));
    // Chrome's intermediate policies can't be expressed by the
    // bool+process-flag pair — typed unservable, never clamped.
    QCOMPARE(invoke(QStringLiteral("network.webRTCIPHandlingPolicy.set"),
                    QJsonObject{{QStringLiteral("details"),
                                 QJsonObject{{QStringLiteral("value"),
                                 QStringLiteral(
                                     "default_public_interface_only")}}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("unsupported"));
    QCOMPARE(invoke(QStringLiteral("network.webRTCIPHandlingPolicy.set"),
                    QJsonObject{{QStringLiteral("details"),
                                 QJsonObject{{QStringLiteral("value"),
                                              7}}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));

    // --- services.passwordSavingEnabled <- autofill/passwordForms -
    QCOMPARE(get(QStringLiteral("services.passwordSavingEnabled"))
                 .value(QLatin1String("value")).toBool(),
             origPasswordForms);
    QVERIFY(setBool(QStringLiteral("services.passwordSavingEnabled"),
                    false).value(QLatin1String("ok")).toBool());
    QCOMPARE(ProfileSettings(profileId).value(
                 QLatin1String("autofill/passwordForms"),
                 true).toBool(), false);

    // --- services.searchSuggestEnabled <- openSearch suggest flag -
    QCOMPARE(get(QStringLiteral("services.searchSuggestEnabled"))
                 .value(QLatin1String("value")).toBool(), origSuggest);
    QVERIFY(setBool(QStringLiteral("services.searchSuggestEnabled"),
                    false).value(QLatin1String("ok")).toBool());
    // The manager write flips the toolbar's live state, not just
    // the persisted row.
    QCOMPARE(ToolbarSearch::openSearchManager()
                 ->suggestionsInAddressField(), false);

    // --- control layering ------------------------------------------
    // A second 'privacy'-declaring extension sees the member the
    // first controls as controlled_by_other_extensions; its clear()
    // is Chrome's silent no-op; its own set() takes the claim (last
    // writer wins).
    const QString dirB = QDir::temp().filePath(
        QStringLiteral("tst-extprivb-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dirB));
    {
        QFile f(dirB + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-priv-b\","
                "\"version\":\"1.0\",\"permissions\":[\"privacy\"]}");
        f.close();
    }
    ExtensionShimBackend backendB(
        QStringLiteral("tstmockextensionid0054"),
        QStringLiteral("tst-priv-b"), dirB);
    QCOMPARE(backendB.invoke(QStringLiteral("privacy"),
                 QStringLiteral(
                     "websites.thirdPartyCookiesAllowed.get"),
                 QJsonObject())
                 .value(QLatin1String("result")).toObject()
                 .value(QLatin1String("levelOfControl")).toString(),
             QStringLiteral("controlled_by_other_extensions"));
    QVERIFY(backendB.invoke(QStringLiteral("privacy"),
                QStringLiteral("websites.thirdPartyCookiesAllowed.clear"),
                QJsonObject())
                .value(QLatin1String("ok")).toBool());
    // The no-op clear changed nothing — backend still controls.
    QCOMPARE(get(QStringLiteral("websites.thirdPartyCookiesAllowed"))
                 .value(QLatin1String("levelOfControl")).toString(),
             QStringLiteral("controlled_by_this_extension"));
    QCOMPARE(get(QStringLiteral("websites.thirdPartyCookiesAllowed"))
                 .value(QLatin1String("value")).toBool(), true);
    QVERIFY(backendB.invoke(QStringLiteral("privacy"),
                QStringLiteral("websites.thirdPartyCookiesAllowed.set"),
                QJsonObject{{QStringLiteral("details"),
                             QJsonObject{{QStringLiteral("value"),
                                          false}}}})
                .value(QLatin1String("ok")).toBool());
    QCOMPARE(ProfileSettings(profileId).value(
                 QLatin1String("cookies/blockThirdPartyCookies"),
                 true).toBool(), true);
    QCOMPARE(backendB.invoke(QStringLiteral("privacy"),
                 QStringLiteral(
                     "websites.thirdPartyCookiesAllowed.get"),
                 QJsonObject())
                 .value(QLatin1String("result")).toObject()
                 .value(QLatin1String("levelOfControl")).toString(),
             QStringLiteral("controlled_by_this_extension"));
    QCOMPARE(get(QStringLiteral("websites.thirdPartyCookiesAllowed"))
                 .value(QLatin1String("levelOfControl")).toString(),
             QStringLiteral("controlled_by_other_extensions"));
    // The new controller's clear restores the pre-extension value —
    // the state before EITHER extension's first set.
    QVERIFY(backendB.invoke(QStringLiteral("privacy"),
                QStringLiteral("websites.thirdPartyCookiesAllowed.clear"),
                QJsonObject())
                .value(QLatin1String("ok")).toBool());
    QCOMPARE(get(QStringLiteral("websites.thirdPartyCookiesAllowed"))
                 .value(QLatin1String("value")).toBool(), orig3pAllowed);

    // --- typed verdicts ---------------------------------------------
    // Published-but-unserved members.
    QCOMPARE(invoke(QStringLiteral("services.translationEnabled.get"))
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("unsupported"));
    QCOMPARE(invoke(QStringLiteral("websites.topicsEnabled.set"),
                    QJsonObject{{QStringLiteral("details"),
                                 QJsonObject{{QStringLiteral("value"),
                                              true}}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("unsupported"));
    QCOMPARE(invoke(QStringLiteral("network.webRTCUDPPortRange.get"))
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("unsupported"));
    // Unknown member / unknown op / malformed names.
    QCOMPARE(invoke(QStringLiteral("websites.bogusMember.get"))
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("unimplemented"));
    QCOMPARE(invoke(QStringLiteral(
                        "websites.thirdPartyCookiesAllowed.frobnicate"))
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("unimplemented"));
    QCOMPARE(invoke(QStringLiteral("websites.thirdPartyCookiesAllowed"))
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("unimplemented"));
    QCOMPARE(invoke(QStringLiteral("get"))
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("unimplemented"));
    // Argument verdicts: mistyped value, missing value, mistyped
    // details/incognito, and the incognito-scope refusals.
    QCOMPARE(invoke(QStringLiteral("websites.thirdPartyCookiesAllowed.set"),
                    QJsonObject{{QStringLiteral("details"),
                                 QJsonObject{{QStringLiteral("value"),
                                              QStringLiteral("yes")}}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    QCOMPARE(invoke(QStringLiteral("websites.thirdPartyCookiesAllowed.set"),
                    QJsonObject{{QStringLiteral("details"),
                                 QJsonObject()}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    QCOMPARE(invoke(QStringLiteral("websites.thirdPartyCookiesAllowed.get"),
                    QJsonObject{{QStringLiteral("details"),
                                 QJsonObject{{QStringLiteral("incognito"),
                                              1}}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    QCOMPARE(invoke(QStringLiteral("websites.thirdPartyCookiesAllowed.set"),
                    QJsonObject{{QStringLiteral("details"),
                                 QJsonObject{{QStringLiteral("value"), true},
                                             {QStringLiteral("scope"),
                                 QStringLiteral(
                                     "incognito_session_only")}}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("unsupported"));
    QCOMPARE(invoke(QStringLiteral("websites.thirdPartyCookiesAllowed.set"),
                    QJsonObject{{QStringLiteral("details"),
                                 QJsonObject{{QStringLiteral("value"), true},
                                             {QStringLiteral("scope"),
                                              QStringLiteral("bogus")}}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));

    // The manifest gate is per-context: a sibling backend without
    // the 'privacy' grant hears no-permission on every op.
    const QString dirC = QDir::temp().filePath(
        QStringLiteral("tst-extprivc-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dirC));
    {
        QFile f(dirC + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-priv-c\","
                "\"version\":\"1.0\",\"permissions\":[\"storage\"]}");
        f.close();
    }
    ExtensionShimBackend backendC(
        QStringLiteral("tstmockextensionid0055"),
        QStringLiteral("tst-priv-c"), dirC);
    QCOMPARE(backendC.invoke(QStringLiteral("privacy"),
                 QStringLiteral("websites.thirdPartyCookiesAllowed.get"),
                 QJsonObject())
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("no-permission"));
    QCOMPARE(backendC.invoke(QStringLiteral("privacy"),
                 QStringLiteral("services.passwordSavingEnabled.set"),
                 QJsonObject{{QStringLiteral("details"),
                              QJsonObject{{QStringLiteral("value"),
                                           true}}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("no-permission"));

    // --- restore the persona -----------------------------------------
    ProfileSettings(profileId).setValue(
        QLatin1String("cookies/blockThirdPartyCookies"), origBlock3p);
    CookieJar::instanceFor(profileId)->loadSettings();
    ProfileSettings(profileId).setValue(
        QLatin1String("privacy/blockPings"), origBlockPings);
    if (origReferer.isValid())
        ProfileSettings(profileId).setValue(
            QLatin1String("privacy/refererPolicy"), origReferer);
    else
        ProfileSettings(profileId).remove(
            QLatin1String("privacy/refererPolicy"));
    ProfileSettings(profileId).setValue(
        QLatin1String("privacy/trimReferer"), origTrimReferer);
    ProfileSettings(profileId).setValue(
        QLatin1String("privacy/gpcSignal"), origGpc);
    ProfileSettings(profileId).setValue(
        QLatin1String("privacy/blockPrefetch"), origBlockPrefetch);
    ProfileSettings(profileId).setValue(
        QLatin1String("privacy/dnsPrefetch"), origDnsPrefetch);
    ProfileSettings(profileId).setValue(
        QLatin1String("privacy/webrtcIpProtection"), origWebrtc);
    ProfileSettings(profileId).setValue(
        QLatin1String("autofill/passwordForms"), origPasswordForms);
    ToolbarSearch::openSearchManager()
        ->setSuggestionsInAddressField(origSuggest);

    QDir(dirC).removeRecursively();
    QDir(dirB).removeRecursively();
    QDir(dir).removeRecursively();
}

// MD18 — chrome.runtime lifecycle ops.  reload rides the manager's
// unload+load of the calling extension; requestUpdateCheck rides
// checkForUpdates/updateResultFor with the verdict pushed back as
// '__arktos.updateCheck' in Chrome's {status,version} vocabulary;
// setUninstallURL persists the page removeExtension() opens once the
// removal actually lands.
void tst_ExtensionManager::extensionRuntimeOps()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");

    const QString dir = QDir::temp().filePath(
        QStringLiteral("tst-extrtops-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dir));
    {
        QFile f(dir + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-rtops\","
                "\"version\":\"1.0\"}");
        f.close();
    }
    ExtensionManager *manager = ExtensionManager::instance();
    auto loaded = std::make_shared<QString>();
    QObject::connect(manager, &ExtensionManager::extensionLoaded,
                     this, [loaded](
                         const ExtensionManager::ExtensionInfo &info) {
        if (info.name == QLatin1String("tst-rtops")
                && info.error.isEmpty())
            *loaded = info.id;
    });
    manager->grantConsent(dir);
    manager->loadExtension(dir);
    QTRY_VERIFY_WITH_TIMEOUT(!loaded->isEmpty(), 30000);
    const QString id = *loaded;

    ExtensionShimBackend backend(id, QStringLiteral("tst-rtops"), dir);
    QSignalSpy pushSpy(&backend, &ExtensionShimBackend::pushEvent);

    // --- setUninstallURL: validation + the persisted roundtrip ----
    const QUrl uninstallPage(
        QStringLiteral("https://example.com/gone?id=1"));
    QJsonObject reply = backend.invoke(
        QStringLiteral("runtime"), QStringLiteral("setUninstallURL"),
        QJsonObject{{QStringLiteral("url"), uninstallPage.toString()}});
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QCOMPARE(manager->uninstallUrlFor(id), uninstallPage);
    // Chrome's contract is http/https only — a script scheme is a
    // typed refusal and the recorded url survives it.
    QCOMPARE(backend.invoke(
                 QStringLiteral("runtime"),
                 QStringLiteral("setUninstallURL"),
                 QJsonObject{{QStringLiteral("url"),
                              QStringLiteral("javascript:alert(1)")}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    QCOMPARE(manager->uninstallUrlFor(id), uninstallPage);
    // An empty url clears the record like Chrome.
    QVERIFY(backend.invoke(
                QStringLiteral("runtime"),
                QStringLiteral("setUninstallURL"),
                QJsonObject{{QStringLiteral("url"), QString()}})
                .value(QLatin1String("ok")).toBool());
    QVERIFY(manager->uninstallUrlFor(id).isEmpty());
    // Re-arm for the removal leg below.
    QVERIFY(backend.invoke(
                QStringLiteral("runtime"),
                QStringLiteral("setUninstallURL"),
                QJsonObject{{QStringLiteral("url"),
                             uninstallPage.toString()}})
                .value(QLatin1String("ok")).toBool());
    QCOMPARE(manager->uninstallUrlFor(id), uninstallPage);

    // --- requestUpdateCheck: Chrome's status vocabulary -----------
    reply = backend.invoke(
        QStringLiteral("runtime"), QStringLiteral("requestUpdateCheck"),
        QJsonObject());
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    // The verdict arrives as a push — the check runs real fetches
    // the invoke reply cannot wait out.  The fixture declares no
    // update_url, so the honest answer is Chrome's "no_update".
    QString updateStatus;
    QElapsedTimer updateTimer;
    updateTimer.start();
    while (updateStatus.isEmpty() && updateTimer.elapsed() < 30000) {
        QTest::qWait(50);
        for (const QList<QVariant> &args : pushSpy) {
            if (args.at(0).toString()
                    != QLatin1String("__arktos.updateCheck"))
                continue;
            updateStatus = args.at(1).toJsonObject()
                .value(QLatin1String("status")).toString();
        }
    }
    QVERIFY((QStringList{QStringLiteral("throttled"),
                         QStringLiteral("no_update"),
                         QStringLiteral("update_available")}
                 .contains(updateStatus)));
    QCOMPARE(updateStatus, QStringLiteral("no_update"));

    // --- reload: unload+load brings the same extension back -------
    auto unloadedCount = std::make_shared<int>(0);
    auto reloadedCount = std::make_shared<int>(0);
    QObject::connect(manager, &ExtensionManager::extensionUnloaded,
                     this, [id, unloadedCount](
                         const ExtensionManager::ExtensionInfo &info) {
        if (info.id == id)
            ++*unloadedCount;
    });
    QObject::connect(manager, &ExtensionManager::extensionLoaded,
                     this, [id, reloadedCount](
                         const ExtensionManager::ExtensionInfo &info) {
        if (info.id == id)
            ++*reloadedCount;
    });
    reply = backend.invoke(
        QStringLiteral("runtime"), QStringLiteral("reload"),
        QJsonObject());
    QVERIFY(reply.value(QLatin1String("ok")).toBool());
    QTRY_VERIFY_WITH_TIMEOUT(*unloadedCount >= 1, 20000);
    QTRY_VERIFY_WITH_TIMEOUT(*reloadedCount >= 1, 30000);
    // The reload's transient unload must NOT open the recorded
    // uninstall page — only a real removal does.
    for (BrowserMainWindow *w
         : BrowserApplication::instance()->mainWindows()) {
        TabWidget *tw = w->tabWidget();
        for (int i = 0; i < tw->count(); ++i)
            QVERIFY2(tw->webView(i)->url() != uninstallPage,
                     "reload-unload opened the uninstall page");
    }
    // A bare backend the manager never tracked gets not-found.
    ExtensionShimBackend bare(
        QStringLiteral("tstmockextensionid0020"),
        QStringLiteral("tst-rtops-bare"), dir);
    QCOMPARE(bare.invoke(QStringLiteral("runtime"),
                         QStringLiteral("reload"), QJsonObject())
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("not-found"));

    // --- removal opens the recorded uninstall page -----------------
    // The page lands in a new tab on whatever window the app fronts —
    // enumerate rather than assuming which registered window got it.
    const auto pageOpened = [uninstallPage]() {
        for (BrowserMainWindow *w
             : BrowserApplication::instance()->mainWindows()) {
            TabWidget *tw = w->tabWidget();
            for (int i = 0; i < tw->count(); ++i) {
                if (tw->webView(i)->url() == uninstallPage)
                    return true;
            }
        }
        return false;
    };
    QVERIFY(!pageOpened());
    manager->removeExtension(id);
    QTRY_VERIFY_WITH_TIMEOUT(pageOpened(), 20000);
    // Consumed — the record dies with the extension.
    QVERIFY(manager->uninstallUrlFor(id).isEmpty());

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
    // The ER16 per-frame fixture: 'scripting' granted, host coverage
    // pinned to /covered/* — a page under it frames /other/frame, an
    // uncovered target living inside an injectable tab.
    const QString dirC = QDir::temp().filePath(
        QStringLiteral("tst-extscript-c-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dirC));
    {
        QFile f(dirC + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-script-c\","
                "\"version\":\"1.0\",\"permissions\":[\"storage\","
                "\"scripting\"],\"host_permissions\":"
                "[\"http://127.0.0.1/covered/*\"],"
                "\"background\":{\"service_worker\":\"sw.js\"}}");
        f.close();
        QFile w(dirC + QLatin1String("/sw.js"));
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

    auto loadedC = std::make_shared<QString>();
    QObject::connect(manager, &ExtensionManager::extensionLoaded,
                     this, [loadedC](
                         const ExtensionManager::ExtensionInfo &info) {
        if (info.name == QLatin1String("tst-script-c")
                && info.error.isEmpty())
            *loadedC = info.id;
    });
    manager->grantConsent(dirC);
    manager->loadExtension(dirC);
    QTRY_VERIFY_WITH_TIMEOUT(!loadedC->isEmpty(), 30000);
    const QString idC = *loadedC;
    manager->setExtensionEnabled(idC, true);
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime->host(idC)
            && runtime->host(idC)->state()
                   == ExtensionBackgroundHost::State::Ready,
        45000);
    ExtensionBackgroundHost *hostC = runtime->host(idC);
    QVERIFY(hostC && hostC->bridge());
    ChromeBridge *bridgeC = hostC->bridge();

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
                req.contains("GET /covered/page")
                    ? QByteArray("<html><head><title>covered-top"
                                 "</title></head><body>top"
                                 "<iframe src=\"/other/frame\">"
                                 "</iframe></body></html>")
                    : req.contains("GET /other/frame")
                        ? QByteArray("<html><head><title>other-doc"
                                     "</title></head><body>o</body>"
                                     "</html>")
                        : req.contains("GET /frame")
                            ? QByteArray("<html><head><title>"
                                         "frame-doc</title></head>"
                                         "<body>f</body></html>")
                            : QByteArray("<html><head><title>"
                                         "exec-target</title></head>"
                                         "<body>exec"
                                         "<iframe src=\"/frame\">"
                                         "</iframe></body></html>");
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

    // --- ER16: per-frame coverage -------------------------------------
    // Fixture C holds 'scripting' but host coverage only for
    // /covered/* — /covered/page's subframe (/other/frame) is an
    // uncovered document living inside a covered tab.  The page-level
    // gate alone would inject into it; every addressed frame must
    // re-prove the grant.
    WebView *view4 = tw->makeNewTab(false);
    QVERIFY(view4 && view4->page());
    QSignalSpy loadSpy4(view4->page(), &QWebEnginePage::loadFinished);
    view4->load(QUrl(base + QStringLiteral("/covered/page")));
    QTRY_VERIFY_WITH_TIMEOUT(loadSpy4.count() >= 1, 30000);
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(view4->page(), QStringLiteral("window.length")).toInt()
            >= 1,
        15000);
    const int tabId4 = ExtensionShimBackend::tabIdFor(view4);
    QVERIFY(tabId4 > 0);

    // Explicit frameIds naming the uncovered frame reject the WHOLE
    // call — Chrome's frameIds contract — before a byte of source
    // exists.  Driven through the shim so the typed failure also
    // lands on the gap ledger.
    eval(hostC->page(), QStringLiteral(
        "self.__exC=undefined;"
        "chrome.scripting.executeScript("
        "{target:{tabId:%1,frameIds:[1]},"
        "func:function(){window.__exMarkC=1;return 1;}},"
        "function(r){self.__exC={r:(r===undefined)?'undef':'def',"
        "err:(window.__arktosShim&&__arktosShim.lastError"
        "   &&__arktosShim.lastError.message)||null};});0")
        .arg(tabId4));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(hostC->page(),
             QStringLiteral("self.__exC!==undefined")).toBool(),
        20000);
    const QJsonObject exC = QJsonDocument::fromJson(
        eval(hostC->page(),
             QStringLiteral("JSON.stringify(self.__exC)"))
            .toString().toUtf8()).object();
    QCOMPARE(exC.value(QLatin1String("r")).toString(),
             QStringLiteral("undef"));
    QVERIFY(exC.value(QLatin1String("err")).toString()
                .contains(QStringLiteral("no-host-access")));

    // Same refusal direct-driven.
    QCOMPARE(bridgeC->invoke(
                 QStringLiteral("scripting"),
                 QStringLiteral("executeScript"),
                 QJsonObject{{QStringLiteral("execId"),
                              idC + QStringLiteral("#fc1")},
                             {QStringLiteral("target"),
                              QJsonObject{{QStringLiteral("tabId"),
                                           tabId4},
                                          {QStringLiteral("frameIds"),
                                           QJsonArray{1}}}},
                             {QStringLiteral("func"),
                              QStringLiteral("function(){}")}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("no-host-access"));

    // allFrames sweeps the uncovered frame in as a per-frame denial
    // entry — the func runs only on the covered document.
    QSignalSpy execSpy(bridgeC, &ChromeBridge::hostEvent);
    QVERIFY(bridgeC->invoke(
        QStringLiteral("scripting"), QStringLiteral("executeScript"),
        QJsonObject{{QStringLiteral("execId"),
                     idC + QStringLiteral("#fc-all")},
                    {QStringLiteral("target"),
                     QJsonObject{{QStringLiteral("tabId"), tabId4},
                                 {QStringLiteral("allFrames"), true}}},
                    {QStringLiteral("func"),
                     QStringLiteral(
                         "function(){window.__exMarkC=1;"
                         "return location.pathname;}")}})
        .value(QLatin1String("ok")).toBool());
    QJsonArray allResults;
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            for (const QList<QVariant> &row : execSpy) {
                if (row.at(0).toString()
                        != QLatin1String("__arktos.execResult"))
                    continue;
                const QJsonObject payload = row.at(1).toJsonObject();
                if (payload.value(QLatin1String("execId")).toString()
                        != idC + QStringLiteral("#fc-all"))
                    continue;
                allResults = payload.value(QLatin1String("results"))
                                 .toArray();
                return true;
            }
            return false;
        }(), 15000);
    QCOMPARE(allResults.size(), 2);
    QCOMPARE(allResults.at(0).toObject()
                 .value(QLatin1String("frameId")).toInt(), 0);
    QCOMPARE(allResults.at(0).toObject()
                 .value(QLatin1String("result")).toString(),
             QStringLiteral("/covered/page"));
    QCOMPARE(allResults.at(1).toObject()
                 .value(QLatin1String("frameId")).toInt(), 1);
    QCOMPARE(allResults.at(1).toObject()
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("no-host-access"));

    // Nothing was generated into the denied frame — the func's marker
    // exists only on the covered top document's agent world.
    auto frameMark = [](QWebEngineFrame frame) {
        auto result = std::make_shared<QVariant>();
        frame.runJavaScript(
            QStringLiteral("window.__exMarkC===1"),
            ExtensionBackgroundRuntime::AgentWorldId,
            [result](const QVariant &v) { *result = v; });
        QElapsedTimer t;
        t.start();
        while (!result->isValid() && t.elapsed() < 10000)
            QTest::qWait(50);
        return result->toBool();
    };
    const QWebEngineFrame topFrame = view4->page()->mainFrame();
    QVERIFY(topFrame.isValid());
    QVERIFY(frameMark(topFrame));
    const QList<QWebEngineFrame> kids = topFrame.children();
    QVERIFY(!kids.isEmpty());
    QVERIFY(!frameMark(kids.at(0)));

    // The typed refusal lands on C's gap ledger too.
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            for (const QString &g : hostC->shimGaps()) {
                if (g.contains(QStringLiteral("no-host-access")))
                    return true;
            }
            return false;
        }(), 15000);

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
    manager->setExtensionEnabled(idC, false);
    QTRY_VERIFY_WITH_TIMEOUT(!runtime->host(id), 20000);
    QTRY_VERIFY_WITH_TIMEOUT(!runtime->host(idB), 20000);
    QTRY_VERIFY_WITH_TIMEOUT(!runtime->host(idC), 20000);
    manager->removeExtension(id);
    manager->removeExtension(idB);
    manager->removeExtension(idC);
    QDir(dir).removeRecursively();
    QDir(dirB).removeRecursively();
    QDir(dirC).removeRecursively();
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

// MD32 — chrome.userScripts over the ER09 registry: register /
// unregister / getScripts / update plus the world-config members.
// A fixture declaring 'userScripts' + loopback host coverage drives
// the real calls from its hosted background: a registered spec
// fires on the next matching navigation inside the shared agent
// world (never the page's own world), update swaps its source,
// unregister stops it, a disable/enable cycle re-arms the (always
// persisted) spec, and configureWorld's record echoes back through
// getWorldConfigurations.  The gate legs prove the 'userScripts'
// permission gate, per-match host-coverage implication, and the
// typed refusals (invalid-argument for missing ids/bare-string js,
// bad-path/no-such-file, no-host-access, not-found, unsupported for
// glob matchers) all BEFORE a byte is armed.
void tst_ExtensionManager::extensionUserScripts()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");

    const QString dir = QDir::temp().filePath(
        QStringLiteral("tst-extus-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dir));
    {
        QFile f(dir + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-us\","
                "\"version\":\"1.0\",\"permissions\":[\"storage\","
                "\"userScripts\",\"scripting\"],\"host_permissions\":"
                "[\"http://127.0.0.1/*\"],\"background\":"
                "{\"service_worker\":\"sw.js\"}}");
        f.close();
        QFile w(dir + QLatin1String("/sw.js"));
        QVERIFY(w.open(QIODevice::WriteOnly));
        w.write(
            "chrome.runtime.onInstalled.addListener(function(){});\n");
        w.close();
        QFile a(dir + QLatin1String("/us_a.js"));
        QVERIFY(a.open(QIODevice::WriteOnly));
        a.write("window.__usA=(window.__usA||0)+1;\n");
        a.close();
        QFile b(dir + QLatin1String("/us_b.js"));
        QVERIFY(b.open(QIODevice::WriteOnly));
        b.write("window.__usB=(window.__usB||0)+1;\n");
        b.close();
    }
    // The consent fixture: identical coverage, no 'userScripts'
    // permission — every member must refuse.
    const QString dirB = QDir::temp().filePath(
        QStringLiteral("tst-extus-b-%1")
            .arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dirB));
    {
        QFile f(dirB + QLatin1String("/manifest.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"manifest_version\":3,\"name\":\"tst-us-b\","
                "\"version\":\"1.0\",\"permissions\":[\"storage\"],"
                "\"host_permissions\":[\"http://127.0.0.1/*\"],"
                "\"background\":{\"service_worker\":\"sw.js\"}}");
        f.close();
        QFile w(dirB + QLatin1String("/sw.js"));
        QVERIFY(w.open(QIODevice::WriteOnly));
        w.write(
            "chrome.runtime.onInstalled.addListener(function(){});\n");
        w.close();
        QFile a(dirB + QLatin1String("/us_a.js"));
        QVERIFY(a.open(QIODevice::WriteOnly));
        a.write("window.__usA=(window.__usA||0)+1;\n");
        a.close();
    }

    ExtensionManager *manager = ExtensionManager::instance();
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();
    auto loaded = std::make_shared<QString>();
    QObject::connect(manager, &ExtensionManager::extensionLoaded,
                     this, [loaded](
                         const ExtensionManager::ExtensionInfo &info) {
        if (info.name == QLatin1String("tst-us")
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
        if (info.name == QLatin1String("tst-us-b")
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
                "<html><head><title>us-target</title></head>"
                "<body>us</body></html>";
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
    auto navigate = [&](const QString &path) {
        loadSpy.clear();
        view->load(QUrl(base + path));
        QTRY_VERIFY_WITH_TIMEOUT(loadSpy.count() >= 1, 30000);
    };

    // --- register + getScripts (the MV3 shapes) -------------------
    eval(host->page(), QStringLiteral(
        "self.__u1=undefined;"
        "chrome.userScripts.register([{id:'us1',"
        "matches:['http://127.0.0.1/*'],js:[{file:'us_a.js'}],"
        "runAt:'document_start'}])"
        ".then(function(){self.__u1='ok';},"
        "function(e){self.__u1='err:'+String(e&&e.message||e);});0"));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(host->page(), QStringLiteral("self.__u1")).toString()
            == QLatin1String("ok"),
        20000);
    eval(host->page(), QStringLiteral(
        "self.__u2=undefined;"
        "chrome.userScripts.getScripts()"
        ".then(function(l){self.__u2=JSON.stringify(l);},"
        "function(e){self.__u2='err';});0"));
    QTRY_VERIFY_WITH_TIMEOUT(
        !eval(host->page(), QStringLiteral("self.__u2")).isNull(),
        20000);
    const QJsonArray listed = QJsonDocument::fromJson(
        eval(host->page(), QStringLiteral("self.__u2"))
            .toString().toUtf8()).array();
    QCOMPARE(listed.size(), 1);
    QCOMPARE(listed.at(0).toObject()
                 .value(QLatin1String("id")).toString(),
             QStringLiteral("us1"));
    // The echo keeps the ExtensionFileSource object shape and the
    // USER_SCRIPT world vocabulary.
    QCOMPARE(listed.at(0).toObject()
                 .value(QLatin1String("js")).toArray().at(0)
                 .toObject().value(QLatin1String("file")).toString(),
             QStringLiteral("us_a.js"));
    QCOMPARE(listed.at(0).toObject()
                 .value(QLatin1String("world")).toString(),
             QStringLiteral("USER_SCRIPT"));

    // The fronts do not see each other's registrations — a
    // scripting-front spec never surfaces in userScripts.getScripts.
    QCOMPARE(bridge->invoke(
                 QStringLiteral("scripting"),
                 QStringLiteral("registerContentScripts"),
                 QJsonObject{{QStringLiteral("scripts"), QJsonArray{
                     QJsonObject{{QStringLiteral("id"),
                                  QStringLiteral("sreg")},
                                 {QStringLiteral("matches"),
                                  QJsonArray{QStringLiteral(
                                      "http://127.0.0.1/*")}},
                                 {QStringLiteral("js"),
                                  QJsonArray{QStringLiteral(
                                      "us_b.js")}}}}}})
                 .value(QLatin1String("ok")).toBool(),
             true);
    eval(host->page(), QStringLiteral(
        "self.__u3=undefined;"
        "chrome.userScripts.getScripts()"
        ".then(function(l){self.__u3=JSON.stringify("
        "l.map(function(s){return s.id;}));},"
        "function(e){self.__u3='err';});0"));
    QTRY_VERIFY_WITH_TIMEOUT(
        !eval(host->page(), QStringLiteral("self.__u3")).isNull(),
        20000);
    QCOMPARE(eval(host->page(), QStringLiteral("self.__u3"))
                 .toString(),
             QStringLiteral("[\"us1\"]"));
    bridge->invoke(
        QStringLiteral("scripting"),
        QStringLiteral("unregisterContentScripts"),
        QJsonObject{{QStringLiteral("filter"),
                     QJsonObject{{QStringLiteral("ids"),
                                  QJsonArray{QStringLiteral(
                                      "sreg")}}}}});

    // --- the registered spec fires in the agent realm -------------
    navigate(QStringLiteral("/us?r1"));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(view->page(), QStringLiteral("window.__usA"),
             agentWorld).toInt() == 1,
        15000);
    QCOMPARE(eval(view->page(), QStringLiteral("window.__usA"))
                 .toInt(), 0);

    // --- update swaps the spec's source ---------------------------
    eval(host->page(), QStringLiteral(
        "self.__u4=undefined;"
        "chrome.userScripts.update([{id:'us1',"
        "js:[{code:'window.__usB=(window.__usB||0)+1;'}]}])"
        ".then(function(){self.__u4='ok';},"
        "function(e){self.__u4='err:'+String(e&&e.message||e);});0"));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(host->page(), QStringLiteral("self.__u4")).toString()
            == QLatin1String("ok"),
        20000);
    navigate(QStringLiteral("/us?u1"));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(view->page(), QStringLiteral("window.__usB"),
             agentWorld).toInt() == 1,
        15000);

    // --- world config: recorded, echoable, resettable -------------
    eval(host->page(), QStringLiteral(
        "self.__w1=undefined;"
        "chrome.userScripts.configureWorld("
        "{messaging:true,worldId:'w1'})"
        ".then(function(){self.__w1='ok';},"
        "function(e){self.__w1='err';});0"));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(host->page(), QStringLiteral("self.__w1")).toString()
            == QLatin1String("ok"),
        20000);
    eval(host->page(), QStringLiteral(
        "self.__w2=undefined;"
        "chrome.userScripts.getWorldConfigurations()"
        ".then(function(l){self.__w2=JSON.stringify(l);},"
        "function(e){self.__w2='err';});0"));
    QTRY_VERIFY_WITH_TIMEOUT(
        !eval(host->page(), QStringLiteral("self.__w2")).isNull(),
        20000);
    const QJsonArray worlds = QJsonDocument::fromJson(
        eval(host->page(), QStringLiteral("self.__w2"))
            .toString().toUtf8()).array();
    QCOMPARE(worlds.size(), 1);
    QCOMPARE(worlds.at(0).toObject()
                 .value(QLatin1String("worldId")).toString(),
             QStringLiteral("w1"));
    QCOMPARE(worlds.at(0).toObject()
                 .value(QLatin1String("messaging")).toBool(),
             true);
    eval(host->page(), QStringLiteral(
        "self.__w3=undefined;"
        "chrome.userScripts.resetWorldConfiguration('w1')"
        ".then(function(){return "
        "chrome.userScripts.getWorldConfigurations();})"
        ".then(function(l){self.__w3=JSON.stringify(l);},"
        "function(e){self.__w3='err';});0"));
    QTRY_VERIFY_WITH_TIMEOUT(
        !eval(host->page(), QStringLiteral("self.__w3")).isNull(),
        20000);
    QCOMPARE(eval(host->page(), QStringLiteral("self.__w3"))
                 .toString(),
             QStringLiteral("[]"));

    // --- persistence: registered user scripts always re-arm -------
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
    QVERIFY(runtime->registeredScriptSpecs(id)
                .contains(QStringLiteral("us1")));
    navigate(QStringLiteral("/us?p1"));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(view->page(), QStringLiteral("window.__usB"),
             agentWorld).toInt() == 1,
        15000);

    // --- unregister stops the next navigation ---------------------
    eval(host->page(), QStringLiteral(
        "self.__u5=undefined;"
        "chrome.userScripts.unregister({ids:['us1']})"
        ".then(function(){self.__u5='ok';},"
        "function(e){self.__u5='err';});0"));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(host->page(), QStringLiteral("self.__u5")).toString()
            == QLatin1String("ok"),
        20000);
    navigate(QStringLiteral("/us?r2"));
    QTest::qWait(1500);
    QCOMPARE(eval(view->page(), QStringLiteral("window.__usB"),
                  agentWorld).toInt(), 0);

    // --- the typed gates, direct-driven ---------------------------
    const auto usReg = [bridge](const QJsonObject &spec) {
        return bridge->invoke(
            QStringLiteral("userScripts"),
            QStringLiteral("register"),
            QJsonObject{{QStringLiteral("scripts"),
                         QJsonArray{spec}}});
    };
    // A caller id is required on the MV3 surface.
    QCOMPARE(usReg(QJsonObject{
                 {QStringLiteral("matches"),
                  QJsonArray{QStringLiteral("http://127.0.0.1/*")}},
                 {QStringLiteral("js"),
                  QJsonArray{QJsonObject{
                      {QStringLiteral("file"),
                       QStringLiteral("us_a.js")}}}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    // js[] is ExtensionFileSource[] — bare path strings refuse.
    QCOMPARE(usReg(QJsonObject{
                 {QStringLiteral("id"), QStringLiteral("bad1")},
                 {QStringLiteral("matches"),
                  QJsonArray{QStringLiteral("http://127.0.0.1/*")}},
                 {QStringLiteral("js"),
                  QJsonArray{QStringLiteral("us_a.js")}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("invalid-argument"));
    // Uncovered match pattern — refused before anything is armed.
    QCOMPARE(usReg(QJsonObject{
                 {QStringLiteral("id"), QStringLiteral("bad2")},
                 {QStringLiteral("matches"),
                  QJsonArray{QStringLiteral(
                      "https://elsewhere.example/*")}},
                 {QStringLiteral("js"),
                  QJsonArray{QJsonObject{
                      {QStringLiteral("file"),
                       QStringLiteral("us_a.js")}}}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("no-host-access"));
    QCOMPARE(usReg(QJsonObject{
                 {QStringLiteral("id"), QStringLiteral("bad3")},
                 {QStringLiteral("matches"),
                  QJsonArray{QStringLiteral("http://127.0.0.1/*")}},
                 {QStringLiteral("js"),
                  QJsonArray{QJsonObject{
                      {QStringLiteral("file"),
                       QStringLiteral("../manifest.json")}}}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("bad-path"));
    QCOMPARE(usReg(QJsonObject{
                 {QStringLiteral("id"), QStringLiteral("bad4")},
                 {QStringLiteral("matches"),
                  QJsonArray{QStringLiteral("http://127.0.0.1/*")}},
                 {QStringLiteral("js"),
                  QJsonArray{QJsonObject{
                      {QStringLiteral("file"),
                       QStringLiteral("missing.js")}}}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("no-such-file"));
    // includeGlobs is declared on the real shape but is a different
    // matcher — typed unsupported, never half-matched.
    QCOMPARE(usReg(QJsonObject{
                 {QStringLiteral("id"), QStringLiteral("bad5")},
                 {QStringLiteral("matches"),
                  QJsonArray{QStringLiteral("http://127.0.0.1/*")}},
                 {QStringLiteral("includeGlobs"),
                  QJsonArray{QStringLiteral("*x*")}},
                 {QStringLiteral("js"),
                  QJsonArray{QJsonObject{
                      {QStringLiteral("file"),
                       QStringLiteral("us_a.js")}}}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("unsupported"));
    QVERIFY(!runtime->registeredScriptSpecs(id)
                 .contains(QStringLiteral("bad5")));
    QCOMPARE(bridge->invoke(
                 QStringLiteral("userScripts"),
                 QStringLiteral("unregister"),
                 QJsonObject{{QStringLiteral("filter"),
                              QJsonObject{{QStringLiteral("ids"),
                                           QJsonArray{QStringLiteral(
                                               "nope")}}}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("not-found"));
    QCOMPARE(bridge->invoke(
                 QStringLiteral("userScripts"),
                 QStringLiteral("update"),
                 QJsonObject{{QStringLiteral("scripts"), QJsonArray{
                     QJsonObject{{QStringLiteral("id"),
                                  QStringLiteral("nope")},
                                 {QStringLiteral("js"),
                                  QJsonArray{QJsonObject{
                                      {QStringLiteral("file"),
                                       QStringLiteral(
                                           "us_a.js")}}}}}}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("not-found"));
    QCOMPARE(bridge->invoke(
                 QStringLiteral("userScripts"),
                 QStringLiteral("bogus"), QJsonObject())
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("unimplemented"));

    // Consent gates: every member needs the 'userScripts' grant.
    QCOMPARE(bridgeB->invoke(
                 QStringLiteral("userScripts"),
                 QStringLiteral("register"),
                 QJsonObject{{QStringLiteral("scripts"), QJsonArray{
                     QJsonObject{{QStringLiteral("id"),
                                  QStringLiteral("bx")},
                                 {QStringLiteral("matches"),
                                  QJsonArray{QStringLiteral(
                                      "http://127.0.0.1/*")}},
                                 {QStringLiteral("js"),
                                  QJsonArray{QJsonObject{
                                      {QStringLiteral("file"),
                                       QStringLiteral(
                                           "us_a.js")}}}}}}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("no-permission"));
    QCOMPARE(bridgeB->invoke(
                 QStringLiteral("userScripts"),
                 QStringLiteral("configureWorld"),
                 QJsonObject{{QStringLiteral("properties"),
                              QJsonObject{{QStringLiteral("messaging"),
                                           true}}}})
                 .value(QLatin1String("error")).toString(),
             QStringLiteral("no-permission"));

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
    // ER12: the real bundle may create notifications — the stub
    // presenter keeps banners off the test display.
    qputenv("ARKTOS_EXTNOTIFY_STUB", "1");

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
    qputenv("ARKTOS_EXTNOTIFY_STUB", "1");
    QVERIFY(QFile::exists(src + QLatin1String("/manifest.json")));

    // Fixture copy — installing (and the hosted runtime's storage
    // writes) must never touch the live profile's bundle.
    const QString copyRoot = QDir::temp().filePath(
        QStringLiteral("tst-bwgap-%1")
            .arg(QCoreApplication::applicationPid()));
    // Unique basename — a crashed earlier run's leftover
    // "bundle_<profile suffix>" install dir makes every later
    // install fail with 'Failed to create install directory'.
    const QString dir = copyRoot + QStringLiteral("/bundle-%1")
            .arg(QCoreApplication::applicationPid());
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
    // ER18: the leg starts windowless; the bundle's own tabs.create
    // call now opens a real window (invokeTabs falls back to
    // newMainWindow like Chrome).  Snapshot the set so teardown can
    // close only the windows the extension itself spawned.
    const QList<QPointer<BrowserMainWindow>> preWindows = [&]() {
        QList<QPointer<BrowserMainWindow>> out;
        for (BrowserMainWindow *w : ExtensionEventHub::browsingWindows())
            out.append(w);
        return out;
    }();

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
    {
        int spawned = 0;
        for (BrowserMainWindow *w : ExtensionEventHub::browsingWindows())
            if (!preWindows.contains(w))
                ++spawned;
        report << QStringLiteral(
            "windows created by extension calls: %1").arg(spawned);
    }
    report << QString();
    // ER18 residual sweep — every emitted typed gap must already be
    // documented in .devin/EXT-RUNTIME-GAPS.md; an unlisted entry is
    // an unplanned regression and fails the leg.
    const QStringList allowedGaps = documentedGaps();
    QStringList unplanned;
    for (const QString &g : gaps)
        if (!gapDocumented(allowedGaps, g))
            unplanned.append(g);
    report << QStringLiteral(
        "## Typed shim gaps — the lane work order (%1 unique, %2 "
        "undocumented)")
        .arg(gaps.size()).arg(unplanned.size());
    for (const QString &g : gaps)
        report << QStringLiteral("- %1%2").arg(
            g, gapDocumented(allowedGaps, g)
                   ? QStringLiteral("  [documented]")
                   : QStringLiteral("  [UNPLANNED]"));
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
    // Close the windows the bundle's own calls spawned (tabs.create
    // on a windowless session mints a real one) — they outlive the
    // extension host and would leak into later legs.  deleteLater,
    // not close(): closeEvent prompts (confirmClosingMultipleTabs
    // defaults on) and a modal box under offscreen hangs forever.
    for (BrowserMainWindow *w : ExtensionEventHub::browsingWindows()) {
        if (!preWindows.contains(w))
            w->deleteLater();
    }
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    manager->setExtensionEnabled(id, false);
    QTRY_VERIFY_WITH_TIMEOUT(!runtime->host(id), 20000);
    manager->removeExtension(id);
    QTest::qWait(500);
    QDir(copyRoot).removeRecursively();

    QVERIFY2(unplanned.isEmpty(),
             qPrintable(QStringLiteral(
                 "undocumented typed gaps emitted: %1 — implement the "
                 "surface or list the signature in "
                 ".devin/EXT-RUNTIME-GAPS.md")
                 .arg(unplanned.join(QLatin1String(" | ")))));
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
    qputenv("ARKTOS_EXTNOTIFY_STUB", "1");
    QVERIFY(QFile::exists(src + QLatin1String("/manifest.json")));

    // Fixture copy — install + runtime storage writes never touch
    // the live profile's bundle.
    const QString copyRoot = QDir::temp().filePath(
        QStringLiteral("tst-bwgate-%1")
            .arg(QCoreApplication::applicationPid()));
    // Unique basename — same install-dir collision guard as the
    // baseline/fill legs.
    const QString dir = copyRoot + QStringLiteral("/bundle-%1")
            .arg(QCoreApplication::applicationPid());
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
    // ER18 residual sweep — the handshake drive's typed gaps are part
    // of the bundle's call set; anything outside
    // .devin/EXT-RUNTIME-GAPS.md's documented set fails below.
    const QStringList hostGaps = host->shimGaps();
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

    const QStringList unplanned = undocumentedGaps(hostGaps);
    QVERIFY2(unplanned.isEmpty(),
             qPrintable(QStringLiteral(
                 "undocumented typed gaps emitted: %1 — implement the "
                 "surface or list the signature in "
                 ".devin/EXT-RUNTIME-GAPS.md")
                 .arg(unplanned.join(QLatin1String(" | ")))));
}

// ER17: the E2E that matters — install -> unlock -> fill, harness
// driven, no human clicks.  The account fixture is Bitwarden's own
// public SDK test account (bitwarden-core test_accounts.rs):
// userId 060000fb-0922-4dd3-b170-6e15cb5df8c8, test@bitwarden.com /
// "asdfasdfasdf", PBKDF2-SHA256 x600k, with the fixture's real
// V1 private-key EncString and master-key-wrapped user key.  We seed
// the state keys Bitwarden reads (global accounts/activeAccountId,
// crypto accountCryptographicState, kdfConfig, the
// masterPasswordUnlock record, a syntactically-valid access token),
// then drive the REAL unlock path —
// bitwardenMain.unlockService.unlockWithMasterPassword — which runs
// the WASM SDK's initialize_user_crypto (real PBKDF2 + AES unwrap).
// The vault fixture is the unlocked-cache route: the decrypted
// cipher record is seeded through cipherService's own state handles
// (the path autofill reads first on every branch), and the fill is
// triggered by the real commands.onCommand("autofill_login")
// dispatch — collectPageDetails -> fillForm -> bootstrap-autofill
// writes the DOM.  The assertion is the page's input.value.
void tst_ExtensionManager::bitwardenUnlockFill()
{
    const QString src = qEnvironmentVariable("ARKTOS_EXTTEST_DIR");
    if (src.isEmpty())
        QSKIP("set ARKTOS_EXTTEST_DIR to an unpacked extension dir");
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");
    qputenv("ARKTOS_EXTNOTIFY_STUB", "1");
    QVERIFY(QFile::exists(src + QLatin1String("/manifest.json")));

    const QString copyRoot = QDir::temp().filePath(
        QStringLiteral("tst-bwfill-%1")
            .arg(QCoreApplication::applicationPid()));
    // Unique basename: Qt names the install dir
    // "<basename>_<profile suffix>", so a crashed earlier run's
    // leftover would make every later install fail with
    // 'Failed to create install directory'.
    const QString dir = copyRoot + QStringLiteral("/bundle-%1")
            .arg(QCoreApplication::applicationPid());
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
    // Bitwarden background.js is ~1.7 MB plus WASM/SDK init
    // and the hosted boot lands anywhere between ~40s and
    // >120s under load — give the ready marker a wide
    // window before declaring the leg dead.
    while (hostTimer.elapsed() < 300000) {
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

    QStringList report;
    report << QStringLiteral(
        "# ER17 — Bitwarden install -> unlock -> fill (harness-driven)");
    report << QStringLiteral("generated: %1").arg(
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    report << QStringLiteral("bundle: %1 name=\"%2\" version=%3")
        .arg(QFileInfo(src).fileName(), manifest.name,
             manifest.version);
    report << QStringLiteral("installed id: %1").arg(id);
    // Live stage markers: the report body only dumps at two points,
    // so a stall between them is invisible in the log otherwise.
    auto stage = [&report](const QString &line) {
        report << line;
        qWarning().noquote() << line;
    };
    stage(QStringLiteral("stage: host ready"));

    // Trace the background's real call surface for the WHOLE run:
    // chrome.tabs.sendMessage (host->page traffic) and
    // chrome.scripting.executeScript (the autofill injection path)
    // both record args + the resolved per-frame results — a throwing
    // injected file lands here as an {error} entry Bitwarden itself
    // ignores, so this is where the fill chain's failures become
    // visible.
    eval(host->page(), QStringLiteral(
        "(function(){self.__er17msgs=[];self.__er17exec=[];"
        "var ts=chrome.tabs.sendMessage;"
        "chrome.tabs.sendMessage=function(id,msg,o,cb){"
        "var rec={cmd:(msg&&msg.command)||'(env)',"
        "resp:'(pending)'};"
        "self.__er17msgs.push(rec);"
        "function done(v){rec.resp=v===undefined?'undef':"
        "(v===null?'null':typeof v==='object'?"
        "'obj:'+JSON.stringify(v).slice(0,180):String(v));}"
        "try{var r=ts.call(this,id,msg,o,function(v){done(v);"
        "if(cb)cb(v);});"
        "if(r&&r.then)r.then(function(v){done(v);})"
        ".catch(function(e){rec.err=String(e&&e.message||e);});"
        "return r;}catch(e){rec.err='throw '+String(e);throw e;}};"
        "var es=chrome.scripting.executeScript;"
        "chrome.scripting.executeScript=function(inj,cb){"
        "var rec={files:(inj&&inj.files)||null,"
        "func:(inj&&inj.func)?'(func)':null,"
        "target:inj&&inj.target,world:inj&&inj.world,"
        "resp:'(pending)'};"
        "self.__er17exec.push(rec);"
        "function done(v){try{rec.resp=JSON.parse("
        "JSON.stringify(v===undefined?null:v));}catch(e){"
        "rec.resp=String(v);}}"
        "try{var r=es.call(this,inj,function(v){done(v);});"
        "if(r&&r.then)r.then(function(v){done(v);})"
        ".catch(function(e){rec.err=String(e&&e.message||e);});"
        "return r;}catch(e){rec.err='throw '+String(e);throw e;}};"
        // Wrap every registered runtime.onMessage listener so the
        // relay dispatch is fully traced: each record keeps the
        // command, the listener's return (true = async sendResponse
        // claimed), and the first sendResponse value.
        "var L=(self.__arktosShim&&__arktosShim.messageListeners)||[];"
        "self.__er17ml=[];self.__er17mlBase=L.length;"
        "for(var li=0;li<L.length;++li){(function(idx,fn){"
        "L[idx]=function(msg,sender,respond){"
        "var rec={i:idx,cmd:(msg&&msg.command)||'(raw)'};"
        "if(self.__er17ml.length<200)self.__er17ml.push(rec);"
        "var r2=function(v){rec.resp=v===undefined?'undef':"
        "(v===null?'null':typeof v==='object'?'obj:'+"
        "JSON.stringify(v).slice(0,140):String(v));"
        "return respond(v);};"
        "try{var r=fn.call(this,msg,sender,r2);"
        "rec.ret=r===true?'true':(r===false?'false':"
        "(r===undefined?'undef':(r&&r.then?'promise':"
        "typeof r)));return r;}catch(e){rec.threw=String(e&&"
        "e.message||e);throw e;}};})(li,L[li]);}"
        "return 0;})()"));


    // Trace the autofill pipeline inside the background: which
    // service call the collectPageDetailsResponse chain reaches, and
    // where it dies (logService.error is Bitwarden's own sink for the
    // orchestrator's dispatch catch — swallowing a throw there is
    // invisible without this tap).
    eval(host->page(), QStringLiteral(
        "(function(){"
        "self.__er17autofill=[];self.__er17err=[];"
        "var tries=0;"
        "var arm=function(){var m=self.bitwardenMain;"
        "if(!m||!m.autofillService)return false;"
        "if(self.__er17armed)return true;"
        "self.__er17armed=1;"
        "var log=function(api,v){self.__er17autofill.push("
        "{api:api,v:v,t:Date.now()});};"
        "if(m&&m.logService){var le=m.logService.error;"
        "m.logService.error=function(e){self.__er17err.push("
        "String(e&&(e.stack||e.message||e)).slice(0,300));"
        "return le.apply(this,arguments);};}"
        "var as=m&&m.autofillService;"
        "if(as){['doAutoFillActiveTab','doAutoFillOnTab','doAutoFill',"
        "'getActiveTab','generateFillScript'].forEach(function(n){"
        "var o=as[n];if(typeof o!=='function')return;"
        "as[n]=function(){var p;try{p=o.apply(this,arguments);"
        "if(n==='generateFillScript'||n==='doAutoFill'){"
        "var pd=arguments[0];"
        "if(pd&&pd.pageDetails&&pd.pageDetails[0])"
        "pd=pd.pageDetails[0].details||pd.pageDetails[0];"
        "log(n+':in',{url:pd&&pd.url,"
        "fields:(pd&&pd.fields||[]).map(function(f){return{"
        "ty:f.type,vw:f.viewable,fn:f.fieldName,id:f.htmlID,"
        "ac:f.autoCompleteType,tag:f.tagName,ph:f.placeholder,"
        "al:f['aria-label'],op:f.opid,fc:f.formCategory,fq:f.fieldQualifier"
        "};})});}"
        "}catch(e){log(n+':threw',String(e&&e.message||e));throw e;}"
        "if(p&&p.then)p.then(function(v){log(n+':done',"
        "v===undefined?'undef':JSON.stringify(v).slice(0,220));})"
        ".catch(function(e){log(n+':rej',String(e&&e.message||e));});"
        "else log(n+':ret',JSON.stringify(p).slice(0,220));"
        "return p;};});}"
        "var cs=m&&m.cipherService;"
        "if(cs){['getLastUsedForUrl','getLastLaunchedForUrl',"
        "'getAllDecryptedForUrl'].forEach(function(n){"
        "var o=cs[n];if(typeof o!=='function')return;"
        "cs[n]=function(u){var p;try{p=o.apply(this,arguments);}"
        "catch(e){log(n+':threw',String(e&&e.message||e));throw e;}"
        "if(p&&p.then)p.then(function(v){log(n+':'+String(u),"
        "v===undefined?'undef':v===null?'null':"
        "(v&&v.length!==undefined?'arr:'+v.length:"
        "'obj:'+JSON.stringify(v&&v.id||v).slice(0,140)));})"
        ".catch(function(e){log(n+':rej',String(e&&e.message||e));});"
        "return p;};});}"
        "return true;};"
        "var tryArm=function(){try{return arm();}catch(e){"
        "self.__er17autofill.push({api:'arm-threw',"
        "e:String(e&&e.message||e)});return false;}};"
        "if(!tryArm()){var iv=setInterval(function(){"
        "if(tryArm()||++tries>360){clearInterval(iv);"
        "if(tries>360)self.__er17autofill.push({api:'arm-timeout',"
        "m:!!self.bitwardenMain,"
        "as:!!(self.bitwardenMain&&self.bitwardenMain.autofillService)});"
        "}},250);}"
        "return 0;})()"));

    // --- the public SDK test-account fixture -------------------------
    const QString uid =
        QStringLiteral("060000fb-0922-4dd3-b170-6e15cb5df8c8");
    const QString email = QStringLiteral("test@bitwarden.com");
    const QString password = QStringLiteral("asdfasdfasdf");
    // The fixture strings stay verbatim so the SDK unwraps the true
    // account material.
    const QString accountCryptoPrivKey = QStringLiteral(
        "2.yN7l00BOlUE0Sb0M//Q53w==|"
        "EwKG/BduQRQ33Izqc/ogoBROIoI5dmgrxSo82sgzgAMIBt3A2FZ9vPRMY+GWT85Jiq"
        "ytDitGR3TqwnFUBhKUpRRAq4x7rA6A1arHrFp5Tp1p21O3SfjtvB3quiOKbqWk6ZaU1"
        "Np9HwqwAecddFcB0YyBEiRX3VwF2pgpAdiPbSMuvo2qIgyob0CUoC/h4Bz1be7Qa7B"
        "0Xw9/fMKkB1LpOm925lzqosyMQM62YpMGkjMsbZz0uPopu32fxzDWSPr+kekNNyLt9"
        "InGhTpxLmq1go/pXR2uw5dfpXc5yuta7DB0EGBwnQ8Vl5HPdDooqOTD9I1jE0mRyuB"
        "pWTTI3FRnu3JUh3rIyGBJhUmHqGZvw2CKdqHCIrQeQkkEYqOeJRJVdBjhv5KGJifqT"
        "3BFRwX/YFJIChAQpebNQKXe/0kPivWokHWwXlDB7S7mBZzhaAPidZvnuIhalE2qmTy"
        "pDwHy22FyqV58T8MGGMchcASDi/QXI6kcdpJzPXSeU9o+NC68QDlOIrMVxKFeE7w7P"
        "vVmAaxEo0YwmuAzzKy9QpdlK0aab/xEi8V4iXj4hGepqAvHkXIQd+r3FNeiLfllkb6"
        "1p6WTjr5urcmDQMR94/wYoilpG5OlybHdbhsYHvIzYoLrC7fzl630gcO6t4nM24vdB"
        "6Ymg9BVpEgKRAxSbE62Tqacxqnz9AcmgItb48NiR/He3n3ydGjPYuKk/ihZMgEwAEZ"
        "vSlNxYONSbYrIGDtOY+8Nbt6KiH3l06wjZW8tcmFeVlWv+tWotnTY9IqlAfvNVTjts"
        "obqtQnvsiDjdEVtNy/s2ci5TH+NdZluca2OVEr91Wayxh70kpM6ib4UGbfdmGgCo74"
        "gtKvKSJU0rTHakQ5L9JlaSDD5FamBRyI0qfL43Ad9qOUZ8DaffDCyuaVyuqk7cz9Hw"
        "mEmvWU3VQ+5t06n/5kRDXttcw8w+3qClEEdGo1KeENcnXCB32dQe3tDTFpuAIMLqwX"
        "s6FhpawfZ5kPYvLPczGWaqftIs/RXJ/EltGc0ugw2dmTLpoQhCqrcKEBDoYVk0LDZK"
        "snzitOGdi9mOWse7Se8798ib1UsHFUjGzISEt6upestxOeupSTOh0v4+AjXbDzRUyo"
        "gHww3V+Bqg71bkcMxtB+WM+pn1XNbVTyl9NR040nhP7KEf6e9ruXAtmrBC2ah5cFEp"
        "LIot77VFZ9ilLuitSz+7T8n1yAh1IEG6xxXxninAZIzi2qGbH69O5RSpOJuJTv17zT"
        "LJQIIc781JwQ2TTwTGnx5wZLbffhCasowJKd2EVcyMJyhz6ru0PvXWJ4hUdkARJs3X"
        "u8dus9a86N8Xk6aAPzBDqzYb1vyFIfBxP0oO8xFHgd30Cgmz8UrSE3qeWRrF8ftrI6"
        "xQnFjHBGWD/JWSvd6YMcQED0aVuQkuNW9ST/DzQThPzRfPUoiL10yAmV7Ytu4fR3x2"
        "sF0Yfi87YhHFuCMpV/DsqxmUizyiJuD938eRcH8hzR/VO53Qo3UIsqOLcyXtTv6THj"
        "SlTopQ+JOLOnHm1w8dzYbLN44OG44rRsbihMUQp+wUZ6bsI8rrOnm9WErzkbQFbrfA"
        "INdoCiNa6cimYIjvvnMTaFWNymqY1vZxGztQiMiHiHYwTfwHTXrb9j0uPM=|"
        "09J28iXv9oWzYtzK2LBT6Yht4IT4MijEkk0fwFdrVQ4=");
    const QString mkwuk = QStringLiteral(
        "2.Q/2PhzcC7GdeiMHhWguYAQ==|"
        "GpqzVdr0go0ug5cZh1n+uixeBC3oC90CIe0hd/HWA/pTRDZ8ane4fmsEIcuc8eMKUt"
        "55Y2q/fbNzsYu41YTZzzsJUSeqVjT8/iTQtgnNdpo=|"
        "dwI+uyvZ1h/iZ03VQ+/wrGEFYVewBUUl/syYgjsNMbE=");

    // --- seed the account state the bundle reads ----------------------
    // Every disk key goes through Bitwarden's own chrome.storage.local
    // envelope ({__json__:true, value:<json>}) so processGetObject
    // unwraps it exactly like a value written by save().
    const QString seedSrc = QStringLiteral(
        "(function(){"
        "var uid='%1',email='%2';"
        "var wrap=function(v){return{__json__:true,"
        "value:JSON.stringify(v)};};"
        "var b64=function(s){return btoa(s).replace(/\\+/g,'-')"
        ".replace(/\\//g,'_').replace(/=+$/,'');};"
        "var jwt=b64('{\"alg\":\"none\",\"typ\":\"JWT\"}')+'.'"
        "+b64(JSON.stringify({exp:4102444800,sub:uid,email:email,"
        "name:'ER17 Test'}))+'.e30';"
        "var items={};"
        "var acc={};acc[uid]={email:email,name:'ER17 Test',"
        "emailVerified:true};"
        "items['global_account_accounts']=wrap(acc);"
        "items['user_'+uid+'_crypto_accountCryptographicState']="
        "wrap({V1:{private_key:'%3'}});"
        "items['user_'+uid+'_kdfConfig_kdfConfig']="
        "wrap({kdfType:0,iterations:600000});"
        "items['user_'+uid+'_masterPasswordUnlock_"
        "masterPasswordUnlockKey']=wrap({salt:email,"
        "kdf:{kdfType:0,iterations:600000},"
        "masterKeyWrappedUserKey:'%4'});"
        "items['user_'+uid+'_masterPassword_"
        "masterKeyEncryptedUserKey']=wrap('%4');"
        "items['user_'+uid+'_token_accessToken']=wrap(jwt);"
        // activeAccountId LAST: MainBackground.init watches the
        // active account and calls switchAccount(null) — wiping this
        // key — if it observes LoggedOut before the auth material
        // above has landed.
        "items['global_account_activeAccountId']=wrap(uid);"
        "self.__er17Seed={done:false};"
        // Bitwarden's observable storage filters onChanged to
        // single-key events — one set() call per key.
        "var keys=Object.keys(items),i=0;"
        "function next(){if(i>=keys.length){self.__er17Seed="
        "{done:true,err:null};return;}"
        "var o={};o[keys[i]]=items[keys[i]];i++;"
        "chrome.storage.local.set(o,function(){"
        "var e=chrome.runtime.lastError;"
        "if(e){self.__er17Seed={done:true,err:e.message||String(e)};"
        "return;}next();});}"
        "next();"
        "return 0;})()")
        .arg(uid, email, accountCryptoPrivKey, mkwuk);
    QVERIFY(eval(host->page(), seedSrc).toInt() == 0);
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(host->page(), QStringLiteral(
            "JSON.stringify(self.__er17Seed)")).toString()
            .contains(QLatin1String("\"done\":true")), 15000);
    const QJsonObject seedResult = QJsonDocument::fromJson(
        eval(host->page(), QStringLiteral(
            "JSON.stringify(self.__er17Seed)")).toString().toUtf8())
        .object();
    report << QStringLiteral("account seed: %1")
        .arg(QString::fromUtf8(
            QJsonDocument(seedResult).toJson(QJsonDocument::Compact)));
    QVERIFY2(seedResult.value(QLatin1String("err")).toVariant().isNull(),
             "storage.local.set rejected");

    // Isolate shim-storage vs state-layer: raw get of what we wrote.
    eval(host->page(), QStringLiteral(
        "(function(){chrome.storage.local.get("
        "['global_account_accounts','global_account_activeAccountId'],"
        "function(v){self.__er17raw={done:true,v:v,"
        "err:(chrome.runtime.lastError&&"
        "chrome.runtime.lastError.message)||null};});return 0;})()"));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(host->page(), QStringLiteral(
            "JSON.stringify(self.__er17raw||null)")).toString()
            .contains(QLatin1String("\"done\":true")), 10000);
    report << QStringLiteral("storage.local readback: %1")
        .arg(eval(host->page(), QStringLiteral(
            "JSON.stringify(self.__er17raw.v)")).toString());

    // Probe the bundle's own storage service (XB over
    // chrome.storage.local) and the chrome.storage.local identity.
    eval(host->page(), QStringLiteral(
        "(function(){var m=self.bitwardenMain;var p={};"
        "p.isShim=(typeof chrome.storage.local.__fire==='function'||"
        "typeof (chrome.storage.local.set&&chrome.storage.local.set"
        ".toString().indexOf('call(')>-1));"
        "p.hasAreaOnChanged=typeof chrome.storage.local.onChanged;"
        "p.hasTopOnChanged=typeof chrome.storage.onChanged;"
        "p.keys=Object.keys(chrome.storage.local).join(',');"
        "self.__er17svcp={done:false,probe:p};"
        "try{m.storageService.get('global_account_accounts')"
        ".then(function(v){self.__er17svcp.done=true;"
        "self.__er17svcp.svcVal=v;})"
        ".catch(function(e){self.__er17svcp.done=true;"
        "self.__er17svcp.svcErr=String(e);});}catch(e){"
        "self.__er17svcp.done=true;self.__er17svcp.svcErr="
        "'sync '+String(e);}"
        "return 0;})()"));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(host->page(), QStringLiteral(
            "JSON.stringify(self.__er17svcp||null)")).toString()
            .contains(QLatin1String("\"done\":true")), 15000);
    report << QStringLiteral("storageService probe: %1")
        .arg(eval(host->page(), QStringLiteral(
            "JSON.stringify(self.__er17svcp)")).toString());

    // Does the real engine fire storage.onChanged for our writes?
    eval(host->page(), QStringLiteral(
        "(function(){self.__er17oc={fired:[]};"
        "chrome.storage.local.onChanged.addListener(function(c){"
        "self.__er17oc.fired.push(Object.keys(c));});"
        "chrome.storage.onChanged.addListener(function(c,a){"
        "self.__er17oc.fired.push('top:'+Object.keys(c));});"
        "var o={};o['er17_probe_key']='x';"
        "chrome.storage.local.set(o,function(){});"
        "return 0;})()"));
    QTest::qWait(1500);
    report << QStringLiteral("onChanged probe: %1")
        .arg(eval(host->page(), QStringLiteral(
            "JSON.stringify(self.__er17oc.fired)")).toString());

    // The state layer must actually SEE the seeded account — read it
    // back through the bundle's own services, not storage.
    eval(host->page(), QStringLiteral(
        "(function(){var m=self.bitwardenMain;if(!m)return 0;"
        "self.__er17acct={done:false};"
        "Promise.all(["
        " new Promise(function(r){m.accountService.accounts$"
        ".subscribe(function(v){r(v)});}),"
        " new Promise(function(r){m.accountService"
        ".activeAccount$.subscribe(function(v){r(v)});})"
        "]).then(function(v){self.__er17acct={done:true,"
        "accounts:v[0],active:v[1]};})"
        ".catch(function(e){self.__er17acct={done:true,"
        "error:String(e)};});"
        "return 0;})()"));
    QTest::qWait(1000);
    const QJsonObject acct = QJsonDocument::fromJson(
        eval(host->page(), QStringLiteral(
            "JSON.stringify(self.__er17acct||null)")).toString()
            .toUtf8()).object();
    report << QStringLiteral("accountService readback: %1")
        .arg(QString::fromUtf8(
            QJsonDocument(acct).toJson(QJsonDocument::Compact)));

    // --- SDK readiness probes — 'not logged in' masks every cause ---
    eval(host->page(), QStringLiteral(
        "(function(){self.__er17sdk={};"
        "self.__er17sdk.href=String(location.href);"
        "self.__er17sdk.wasm=(typeof WebAssembly!=='undefined');"
        "self.__er17sdk.initSdk=(typeof globalThis.initSdk);"
        "var rel='b149282d9595bc31ecc7.module.wasm';"
        "var abs='';"
        "try{abs=chrome.runtime.getURL(rel);}catch(e){}"
        "self.__er17sdk.extUrl=abs;"
        "fetch(rel).then(function(r){self.__er17sdk.relFetch="
        "r.status+' '+r.url;}).catch(function(e){"
        "self.__er17sdk.relFetch='ERR '+String(e);});"
        "if(abs)fetch(abs).then(function(r){self.__er17sdk.absFetch="
        "r.status+' '+r.url;}).catch(function(e){"
        "self.__er17sdk.absFetch='ERR '+String(e);});"
        "return 0;})()"));
    QTest::qWait(3000);
    report << QStringLiteral("sdk probe: %1")
        .arg(eval(host->page(), QStringLiteral(
            "JSON.stringify(self.__er17sdk)")).toString());
    report << QStringLiteral("accountService readback (seen above): "
                             "accounts non-empty=%1")
        .arg(acct.value(QLatin1String("accounts")).toObject()
                 .isEmpty() ? QStringLiteral("NO")
                            : QStringLiteral("yes"));

    // Bitwarden's init calls switchAccount(null) when the active
    // account reads LoggedOut — if that raced our seed, re-assert
    // activeAccountId until the real service sees the account.
    int reasserts = 0;
    {
        QElapsedTimer activeTimer;
        activeTimer.start();
        while (activeTimer.elapsed() < 15000) {
            eval(host->page(), QStringLiteral(
                "(function(){var m=self.bitwardenMain;if(!m)return 0;"
                "self.__er17act={done:false};"
                "var sub=m.accountService.activeAccount$"
                ".subscribe(function(v){if(!self.__er17act.done){"
                "self.__er17act={done:true,active:v};"
                "try{sub.unsubscribe()}catch(e){}}});"
                "setTimeout(function(){self.__er17act.done=true;},"
                "1500);return 0;})()"));
            QTRY_VERIFY_WITH_TIMEOUT(
                eval(host->page(), QStringLiteral(
                    "JSON.stringify(self.__er17act||{})")).toString()
                    .contains(QLatin1String("\"done\":true")), 5000);
            const QJsonObject act = QJsonDocument::fromJson(
                eval(host->page(), QStringLiteral(
                    "JSON.stringify(self.__er17act)")).toString()
                    .toUtf8()).object();
            if (!act.value(QLatin1String("active")).isNull()
                    && !act.value(QLatin1String("active")).isUndefined())
                break;
            ++reasserts;
            eval(host->page(), QStringLiteral(
                "(function(){var o={};"
                "o['global_account_activeAccountId']={__json__:true,"
                "value:JSON.stringify('%1')};"
                "chrome.storage.local.set(o,function(){});"
                "return 0;})()").arg(uid));
            QTest::qWait(700);
        }
    }
    if (reasserts)
        report << QStringLiteral("activeAccountId re-asserted x%1")
            .arg(reasserts);

    // --- REAL unlock through the WASM SDK -----------------------------
    stage(QStringLiteral("stage: unlock issued"));
    eval(host->page(), QStringLiteral(
        "(function(){self.__er17unlock={done:false};"
        "var m=self.bitwardenMain;"
        "if(!m||!m.unlockService){self.__er17unlock={done:true,"
        "error:'bitwardenMain.unlockService missing'};return 0}"
        "m.unlockService.unlockWithMasterPassword('%1','%2')"
        ".then(function(){return m.authService.getAuthStatus();})"
        ".then(function(s){self.__er17unlock={done:true,status:s};})"
        ".catch(function(e){self.__er17unlock={done:true,"
        "error:String(e&&(e.stack||e.message||e))};});"
        "return 0;})()").arg(uid, password));
    // The real unlock call runs through runOnUnlockSideEffects (SDK
    // crypto init, then platform side-effects like biometrics and
    // autoUnlock that have no reachable backend in the harness) and
    // its promise may not settle — but the honest verdict is the
    // account's auth status on the real auth service: seeded
    // unlocked-cache state plus the issued unlock should read as
    // Unlocked (2).
    eval(host->page(), QStringLiteral(
        "(function(){var m=self.bitwardenMain;if(!m)return 0;"
        "self.__er17auth={status:-1,seen:[]};"
        "m.authService.authStatusFor$('%1').subscribe(function(v){"
        "self.__er17auth.status=v;"
        "self.__er17auth.seen.push(v);});"
        "return 0;})()").arg(uid));
    // The stream starts at Locked (1) and flips to Unlocked (2)
    // when the in-flight unlock finishes its SDK work — poll the
    // live value, not the first emission.
    int authStatus = -1;
    {
        QElapsedTimer unlockTimer;
        unlockTimer.start();
        while (unlockTimer.elapsed() < 90000) {
            authStatus = eval(host->page(), QStringLiteral(
                "(self.__er17auth||{}).status")).toInt();
            if (authStatus == 2)
                break;
            QTest::qWait(500);
        }
        report << QStringLiteral("unlock result: %1 auth status: %2"
                                 " (stream: %3)")
            .arg(eval(host->page(), QStringLiteral(
                "JSON.stringify(self.__er17unlock||{})")).toString())
            .arg(authStatus)
            .arg(eval(host->page(), QStringLiteral(
                "JSON.stringify((self.__er17auth||{}).seen||[])"))
                .toString());
    }
    stage(QStringLiteral("stage: unlock verdict"));
    if (authStatus != 2) {
        // Measured gap — name which awaited observable stalls:
        // subscribe to every observable unlockWithMethod touches,
        // each with its own watchdog, and record which emitted.
        eval(host->page(), QStringLiteral(
            "(function(){var m=self.bitwardenMain;if(!m)return 0;"
            "self.__er17diag={done:false,emitted:{},members:[]};"
            "self.__er17diag.members=Object.keys(m).filter("
            "function(k){return /sdk|crypto|kdf|auth|account|unlock"
            "|register|token/.test(k)});"
            "var uid='%1';"
            "function probe(name,obs){try{var done=false;"
            "obs.subscribe(function(v){if(!done){done=true;"
            "self.__er17diag.emitted[name]=v===null?'null':"
            "(v===undefined?'undef':typeof v==='object'?'obj':"
            "String(v)).slice(0,80);}});}catch(e){"
            "self.__er17diag.emitted[name]='THROW '+e;}}"
            "try{probe('registerClient$',"
            "m.registerSdkService.registerClient$(uid));}catch(e){"
            "self.__er17diag.emitted.registerClient='THROW '+e;}"
            "try{probe('accounts$',m.accountService.accounts$);}"
            "catch(e){self.__er17diag.emitted.accounts='THROW '+e;}"
            "try{probe('activeAccount$',"
            "m.accountService.activeAccount$);}"
            "catch(e){self.__er17diag.emitted.activeAccount="
            "'THROW '+e;}"
            "try{probe('kdfConfig$',"
            "m.kdfConfigService.getKdfConfig$(uid));}catch(e){"
            "self.__er17diag.emitted.kdfConfig='THROW '+e;}"
            "try{probe('cryptoState$',"
            "m.accountCryptographicStateService"
            ".accountCryptographicState$(uid));}catch(e){"
            "self.__er17diag.emitted.cryptoState='THROW '+e;}"
            "try{probe('mpUnlockData$',"
            "m.masterPasswordService"
            ".masterPasswordUnlockData$(uid));}catch(e){"
            "self.__er17diag.emitted.mpUnlockData='THROW '+e;}"
            "try{probe('v2UpgradeToken$',"
            "m.v2UpgradeTokenStateService.v2UpgradeToken$(uid));}"
            "catch(e){self.__er17diag.emitted.v2Token='THROW '+e;}"
            "try{probe('authStatus$',"
            "m.authService.authStatusFor$(uid));}catch(e){"
            "self.__er17diag.emitted.authStatus='THROW '+e;}"
            "setTimeout(function(){self.__er17diag.done=true;},"
            "8000);return 0;})()").arg(uid));
        QTRY_VERIFY_WITH_TIMEOUT(
            eval(host->page(), QStringLiteral(
                "JSON.stringify(self.__er17diag||{})")).toString()
                .contains(QLatin1String("\"done\":true")), 20000);
        report << QStringLiteral("diag: %1")
            .arg(eval(host->page(), QStringLiteral(
                "JSON.stringify(self.__er17diag)")).toString());
        for (const QString &line : report)
            qWarning().noquote() << line;
        QVERIFY2(false, qPrintable(QStringLiteral(
            "auth status did not reach Unlocked: %1")
            .arg(authStatus)));
    }

    // --- seed the unlocked vault cache --------------------------------
    stage(QStringLiteral("stage: cipher seed issued"));
    // The autofill read path consults decryptedCiphersState first on
    // every branch (cache or SDK); the fixture cipher is plaintext
    // fixture data — synthetic credentials, never user data.
    const QString cipherId = QStringLiteral(
        "11111111-2222-4333-8444-555555555555");
    const QString fixtureUser = QStringLiteral("er17.user@arktos.test");
    const QString fixturePass = QStringLiteral("Sup3rSecret-ER17!");

    // --- a real login tab on a covered host ---------------------------
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
                "<html><body><form id=loginform>"
                "<input type=text name=username id=username "
                "autocomplete=username>"
                "<input type=password name=password id=password "
                "autocomplete=current-password>"
                "</form></body></html>";
            socket->write(
                "HTTP/1.1 200\r\nContent-Type: text/html\r\n"
                "Content-Length: "
                + QByteArray::number(body.size())
                + "\r\nConnection: close\r\n\r\n" + body);
            socket->disconnectFromHost();
        });
    });
    const QString pageUrl = base + QStringLiteral("/login");

    // decrypted cipher record (CipherView JSON — rb.fromJSON shape)
    const QString cipherView = QStringLiteral(
        "{\"id\":\"%1\",\"type\":1,\"name\":\"ER17 fixture login\","
        "\"organizationId\":null,\"folderId\":null,\"collectionIds\":[],"
        "\"localData\":{},\"notes\":null,\"edit\":true,"
        "\"viewPassword\":true,\"favorite\":false,"
        "\"organizationUseTotp\":false,\"reprompt\":0,"
        "\"attachments\":[],\"fields\":[],\"passwordHistory\":[],"
        "\"login\":{\"username\":\"%2\",\"password\":\"%3\","
        "\"totp\":null,\"uris\":[{\"uri\":\"%4/\",\"match\":1,"
        "\"uriChecksum\":null}],\"fido2Credentials\":[],"
        "\"autofillOnPageLoad\":null,\"passwordRevisionDate\":null},"
        "\"revisionDate\":\"2024-01-01T00:00:00.000Z\","
        "\"creationDate\":\"2024-01-01T00:00:00.000Z\","
        "\"deletedDate\":null,\"archivedDate\":null,"
        "\"decryptionFailure\":false}")
        .arg(cipherId, fixtureUser, fixturePass, base);
    const QString cipherSeed = QStringLiteral(
        "(function(){self.__er17ciphers={done:false};"
        "var m=self.bitwardenMain,cs=m.cipherService,uid='%1';"
        "var view=%2;"
        "Promise.all(["
        " cs.decryptedCiphersState(uid).update(function(){var r={};"
        "  r['%3']=view;return r;}),"
        " cs.encryptedCiphersState(uid).update(function(){return {}})"
        "]).then(function(){self.__er17ciphers={done:true};})"
        ".catch(function(e){self.__er17ciphers={done:true,"
        "error:String(e&&(e.stack||e.message||e))};});"
        "return 0;})()")
        .arg(uid, cipherView, cipherId);
    QVERIFY(eval(host->page(), cipherSeed).toInt() == 0);
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(host->page(), QStringLiteral(
            "JSON.stringify(self.__er17ciphers||{})")).toString()
            .contains(QLatin1String("\"done\":true")), 20000);
    const QJsonObject cipherResult = QJsonDocument::fromJson(
        eval(host->page(), QStringLiteral(
            "JSON.stringify(self.__er17ciphers)")).toString().toUtf8())
        .object();
    report << QStringLiteral("cipher seed: %1")
        .arg(QString::fromUtf8(QJsonDocument(cipherResult)
            .toJson(QJsonDocument::Compact)));
    QVERIFY2(cipherResult.value(QLatin1String("error")).toString()
                 .isEmpty(),
             qPrintable(QStringLiteral(
                 "cipher state seed failed: %1")
                 .arg(QString::fromUtf8(QJsonDocument(cipherResult)
                     .toJson(QJsonDocument::Compact)))));

    stage(QStringLiteral("stage: opening login tab"));
    BrowserMainWindow *window = new BrowserMainWindow;
    // The form fields must be genuinely viewable — Bitwarden's
    // isElementOutsideViewportBounds measures against
    // documentElement.scrollWidth/scrollHeight, which stay 0 until
    // the renderer gets a real compositor viewport; a plain resize()
    // on a never-shown window leaves innerWidth at 0 and every field
    // reports viewable:false (empty fill script -> didAutofill:false).
    // WA_DontShowOnScreen + show() creates the platform window and
    // real geometry without mapping to the (offscreen) display.
    window->resize(1024, 768);
    window->setAttribute(Qt::WA_DontShowOnScreen);
    window->show();
    TabWidget *tw = window->tabWidget();
    QVERIFY(tw && tw->count() >= 1);
    WebView *view = tw->webView(0);
    QVERIFY(view && view->page());
    QSignalSpy loadSpy(view->page(), &QWebEnginePage::loadFinished);
    view->load(QUrl(pageUrl));
    QTRY_VERIFY_WITH_TIMEOUT(loadSpy.count() >= 1, 30000);
    const int tabId = ExtensionShimBackend::tabIdFor(view);
    QVERIFY(tabId > 0);
    // Bitwarden's generateFillScript drops viewable:false fields —
    // record exactly what the page reports so a measured gap names
    // its cause (zero-size viewport vs hidden-document vs layout).
    report << QStringLiteral("page viewport: %1")
        .arg(eval(view->page(), QStringLiteral(
            "(function(){var i=document.querySelector('input');"
            "var r=i?i.getBoundingClientRect():null;"
            "return JSON.stringify({iw:innerWidth,ih:innerHeight,"
            "vis:document.visibilityState,hid:document.hidden,"
            "r:r&&{x:r.x,y:r.y,w:r.width,h:r.height},"
            "ow:i&&i.offsetWidth,oh:i&&i.offsetHeight});})()"))
            .toString());
    // The declared trigger script + the on-demand bootstrap-autofill
    // injection cross before the command can complete.
    const QString probe = QStringLiteral(
        "((window.__arktosExtAgents&&__arktosExtAgents['%1'])"
        "||{active:false,bridgeUp:false})").arg(id);
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(view->page(), probe + QStringLiteral(".bridgeUp"),
             agentWorld).toBool(),
        30000);
    report << QStringLiteral("tab: id=%1 url=%2")
        .arg(tabId).arg(view->page()->url().toString());

    // Content-side instrumentation.  The bootstrap content script is
    // injected by the command itself, so trap the moment the bundle
    // publishes window.bitwardenAutofillInit and wrap the methods the
    // fill chain hangs on: the AutofillInit + CollectAutofill service
    // sendExtensionMessage members (both are instance properties the
    // classes late-bind through 'this.') and getPageDetails — plus a
    // window-level unhandledrejection tap, since an async leg that
    // throws inside the bundle's awaiter surfaces nowhere else.
    const QString spyInstall = QStringLiteral(
        "(function(){"
        "var a=(window.__arktosExtAgents||{})['%1'];"
        "if(!a||!a.chrome)return 'noagent';"
        "window.__er17cs=[];"
        "var push=function(o){try{o.t=Date.now();}catch(e){}"
        "if(window.__er17cs.length<300)window.__er17cs.push(o);};"
        "var rt=a.chrome.runtime;"
        "if(rt&&rt.sendMessage&&!rt.sendMessage.__er17){"
        "var osm=rt.sendMessage;"
        "var wm=function(){var m=arguments[0];"
        "var rec={api:'rt.send',cmd:(m&&m.command)||'(raw)'};"
        "push(rec);try{var r=osm.apply(rt,arguments);"
        "if(r&&r.then)r.then(function(v){rec.resp=v===undefined"
        "?'undef':(v===null?'null':(typeof v==='object'?'obj:'"
        "+JSON.stringify(v).slice(0,120):String(v)));},"
        "function(e){rec.err=String(e&&e.message||e);});"
        "return r;}catch(e){rec.threw=String(e);throw e;}};"
        "wm.__er17=1;"
        "try{rt.sendMessage=wm;}catch(e){push({api:'sendwrap-fail'});}"
        "}"
        "var wrapSext=function(owner,tag){"
        "if(!owner||typeof owner.sendExtensionMessage!=='function'"
        "||owner.sendExtensionMessage.__er17)return;"
        "var ose=owner.sendExtensionMessage;"
        "var f=function(c){var rec={api:tag,cmd:String(c)};push(rec);"
        "try{var r=ose.apply(owner,arguments);"
        "if(r&&r.then)r.then(function(v){rec.resp=v===undefined"
        "?'undef':(v===null?'null':(typeof v==='object'?'obj:'"
        "+JSON.stringify(v).slice(0,120):String(v)));},"
        "function(e){rec.err=String(e&&e.message||e);});"
        "return r;}catch(e){rec.threw=String(e);throw e;}};"
        "f.__er17=1;owner.sendExtensionMessage=f;};"
        "var wrapInst=function(ai){"
        "if(!ai||ai.__er17w)return ai;"
        "try{Object.defineProperty(ai,'__er17w',{value:1});}"
        "catch(e){}"
        "wrapSext(ai,'ai.send');"
        "try{var cs=ai.collectAutofillContentService;"
        "wrapSext(cs,'cs.send');"
        "if(cs&&cs.getPageDetails&&!cs.getPageDetails.__er17){"
        "var og=cs.getPageDetails;"
        "var wg=function(){push({api:'getPageDetails:call'});"
        "var p=og.apply(cs,arguments);"
        "if(p&&p.then)p.then(function(v){push({api:'gpd:done',"
        "fields:v&&v.fields?v.fields.length:-1,"
        "flds:(v&&v.fields||[]).map(function(f){return{ty:f.type,"
        "vw:f.viewable,fn:f.fieldName,id:f.htmlID,ac:f.autoCompleteType,"
        "pf:f.maxlength===-1?null:f.maxlength};}).slice(0,6)});},"
        "function(e){push({api:'gpd:err',"
        "e:String(e&&(e.stack||e.message||e)).slice(0,240)});});"
        "return p;};wg.__er17=1;cs.getPageDetails=wg;}"
        "if(cs&&cs.attributeSettingsFetched"
        "&&cs.attributeSettingsFetched.then)"
        "cs.attributeSettingsFetched.then("
        "function(){push({api:'attrSettings:resolved'});},"
        "function(e){push({api:'attrSettings:rejected',"
        "e:String(e&&e.message||e)});});"
        "if(cs&&cs.isMonitoring!==undefined)"
        "push({api:'cs.isMonitoring',v:cs.isMonitoring});"
        "}catch(e){push({api:'wrap-err',e:String(e)});}"
        "return ai;};"
        "try{wrapInst(window.bitwardenAutofillInit);}catch(e){}"
        // held starts at the CURRENT value: the bootstrap file
        // assigns window.bitwardenAutofillInit once before this spy
        // installs, so a bare 'var held' would hide the live
        // instance behind the accessor (reads -> undefined) and
        // break any bundle code that reaches for it later.
        "var held=window.bitwardenAutofillInit;"
        "try{Object.defineProperty(window,'bitwardenAutofillInit',{"
        "configurable:true,get:function(){return held;},"
        "set:function(v){held=wrapInst(v);}});}catch(e){"
        "push({api:'trap-err',e:String(e)});}"
        "window.addEventListener('unhandledrejection',function(ev){"
        "push({api:'unhandledrejection',"
        "e:String(ev.reason&&(ev.reason.stack||ev.reason.message"
        "||ev.reason)).slice(0,300)});});"
        "window.addEventListener('error',function(ev){"
        "push({api:'windowerror',"
        "e:String(ev.message).slice(0,200)});});"
        "push({api:'world',readyState:(document&&document.readyState)"
        "||'?',gIsW:globalThis===window,"
        "href:String(location.href)});"
        "return 'armed';})()")
        .arg(id);
    report << QStringLiteral("cs spy: %1")
        .arg(eval(view->page(), spyInstall, agentWorld).toString());

    // --- the real trigger: commands.onCommand('autofill_login') -------
    // Wait for the bundle's own readiness before firing the user
    // command — the harness compresses minutes of real boot into
    // seconds, and an autofill command drops inside Bitwarden itself
    // when (a) the services are not yet published (fillRequest$ is
    // subscribed in init) or (b) autofillLifecycleService.liveTabs
    // does not yet contain the tab — its tabs.onCreated-fed seed
    // resolves asynchronously and the command's fillRequest$ filter
    // reads the set synchronously (measured: the [tabId] emission
    // lands ~12ms AFTER the command's request).  The gate is the
    // extension's own readiness, the peer of host->Ready on our
    // side; its outcome is recorded either way.
    const QString svcReadySrc = QStringLiteral(
        "(function(){var m=self.bitwardenMain;"
        "if(!(m&&m.autofillService&&m.autofillOrchestrator"
        "&&m.cipherService))return false;"
        "var lt=m.autofillLifecycleService"
        "&&m.autofillLifecycleService.liveTabs;"
        "if(!lt)return false;"
        "var s;try{s=lt.getValue?lt.getValue():"
        "(lt.value!==undefined?lt.value:lt._value);}catch(e){"
        "return false;}"
        "return !!(s&&typeof s.has==='function'&&s.has(%1));})()")
        .arg(tabId);
    QElapsedTimer svcGate;
    svcGate.start();
    bool svcReady = false;
    while (svcGate.elapsed() < 120000) {
        if (eval(host->page(), svcReadySrc).toBool()) {
            svcReady = true;
            break;
        }
        QTest::qWait(500);
    }
    report << QStringLiteral("svc readiness gate: %1 (%2 ms)")
        .arg(svcReady ? QStringLiteral("ready")
                      : QStringLiteral("TIMEOUT — services never "
                                       "published"))
        .arg(svcGate.elapsed());
    // Service-side trace of the fill chain: the command path is
    // collectPageDetails -> collectPageDetailsResponse ->
    // autofillOrchestrator.fillRequest$ (filtered on
    // autofillLifecycleService.liveTabs — chrome.tabs.onCreated-fed)
    // -> autofillService.doAutoFillActiveTab -> doAutoFillOnTab ->
    // doAutoFill -> tabSendMessage('fillForm').  Every hop is wrapped
    // so a silent drop names its station.
    eval(host->page(), QStringLiteral(
        "(function(){var m=self.bitwardenMain;if(!m)return 'nom';"
        "self.__er17svc={log:[]};var L=self.__er17svc.log;"
        "var push=function(o){try{o.t=Date.now();}catch(e){}"
        "if(L.length<400)L.push(o);};"
        "try{var lt=m.autofillLifecycleService"
        "&&m.autofillLifecycleService.liveTabs;"
        "if(lt&&lt.subscribe)lt.subscribe(function(s){"
        "push({api:'liveTabs',tabs:Array.from(s)});});"
        "else push({api:'liveTabs',missing:true});}catch(e){"
        "push({api:'liveTabs-sub',err:String(e)});}"
        "try{var orc=m.autofillOrchestrator;"
        "var fr=orc&&orc.fillRequest$;"
        "if(fr&&fr.subscribe)fr.subscribe(function(v){"
        "try{push({api:'fillRequest',kind:v&&v.kind,"
        "tabId:v&&v.tabId,frameId:v&&v.frameId,"
        "fields:(v&&v.pageDetail&&v.pageDetail.details"
        "&&v.pageDetail.details.fields||[]).length});}"
        "catch(e){push({api:'fillRequest',err:String(e)});}});"
        "var od=orc&&orc.dispatch;"
        "if(od)orc.dispatch=function(e){"
        "push({api:'dispatch',kind:e&&e.kind,tabId:e&&e.tabId});"
        "try{var r=od.apply(orc,arguments);"
        "if(r&&r.then)r.then(function(){push({api:'dispatch:done',"
        "kind:e&&e.kind});},function(x){push({api:'dispatch:err',"
        "e:String(x&&(x.stack||x.message||x)).slice(0,300)});});"
        "return r;}catch(x){push({api:'dispatch:threw',"
        "e:String(x)});throw x;}};}catch(e){"
        "push({api:'dispatch-wrap',err:String(e)});}"
        "var svc=m.autofillService;if(svc){"
        "['doAutoFillActiveTab','doAutoFillOnTab','doAutoFill',"
        "'generateFillScript','getActiveTab'].forEach(function(n){"
        "var orig=svc[n];if(typeof orig!=='function'){"
        "push({api:'svc-missing',name:n});return;}"
        "svc[n]=function(){var rec={api:'svc:'+n};"
        "try{if(n==='doAutoFillActiveTab'||n==='doAutoFillOnTab'){"
        "var pd=arguments[0];"
        "var d=pd&&pd[0]&&pd[0].details;"
        "rec.fields=(d&&d.fields||[]).length;"
        "rec.fieldShape=JSON.stringify((d&&d.fields||[])"
        ".map(function(f){return{type:f.type,viewable:f.viewable,"
        "opid:!!f.opid,name:f.htmlName,form:!!f.form,"
        "elnum:f.elementNumber,actype:f.autoCompleteType};}))"
        ".slice(0,400);"
        "rec.durl=d&&d.url;rec.tab=arguments[1]&&arguments[1].id;"
        "rec.tabUrl=arguments[1]&&arguments[1].url;}"
        "if(n==='doAutoFill'){var a=arguments[0];"
        "rec.tab=a&&a.tab&&a.tab.id;"
        "rec.pdt=(a&&a.pageDetails||[]).map(function(x){"
        "return{tid:x&&x.tab&&x.tab.id,"
        "turl:x&&x.tab&&x.tab.url,"
        "durl:x&&x.details&&x.details.url,"
        "nf:(x&&x.details&&x.details.fields||[]).length};});"
        "rec.cipher=a&&a.cipher&&a.cipher.id;}"
        "if(n==='generateFillScript'){var g=arguments[0];"
        "rec.gfields=JSON.stringify((g&&g.fields||[])"
        ".map(function(f){return{type:f.type,viewable:f.viewable,"
        "opid:!!f.opid,name:f.htmlName,form:!!f.form,"
        "elnum:f.elementNumber,targeted:f.targeted};}))"
        ".slice(0,400);"
        "rec.gforms=Object.keys(g&&g.forms||{}).length;"
        "rec.gurl=g&&g.url;}}catch(e){}"
        "push(rec);try{var r=orig.apply(svc,arguments);"
        "if(r&&r.then)r.then(function(v){"
        "push({api:'svc:'+n+':done',v:n==='generateFillScript'?"
        "(v?'script':'null'):(v===undefined?'undef':"
        "(v===null?'null':(typeof v==='object'?"
        "'obj:'+JSON.stringify(v).slice(0,120):String(v))))});},"
        "function(x){push({api:'svc:'+n+':err',"
        "e:String(x&&(x.stack||x.message||x)).slice(0,300)});});"
        "return r;}catch(x){push({api:'svc:'+n+':threw',"
        "e:String(x)});throw x;}};});}"
        "var cs=m.cipherService;if(cs){"
        "['getLastUsedForUrl','getCipherForUrl',"
        "'getAllDecryptedForUrl'].forEach(function(n){"
        "var orig=cs[n];if(typeof orig!=='function')return;"
        "cs[n]=function(){var rec={api:'cipher:'+n,"
        "url:arguments[0]};push(rec);try{var r=orig.apply(cs,"
        "arguments);if(r&&r.then)r.then(function(v){"
        "push({api:'cipher:'+n+':done',"
        "v:Array.isArray(v)?('n='+v.length):"
        "(v===undefined?'undef':(v===null?'null':"
        "(v&&v.id)||String(v)))});},function(x){"
        "push({api:'cipher:'+n+':err',"
        "e:String(x&&(x.message||x)).slice(0,200)});});"
        "return r;}catch(x){push({api:'cipher:'+n+':threw',"
        "e:String(x)});throw x;}};});}"
        "return 'svc-spies-armed';})()"));

    stage(QStringLiteral("stage: autofill_login command fired"));
    ExtensionCommandRegistry::instance()->trigger(
        id, QStringLiteral("autofill_login"), window);

    // Assert on the DOM, not the plumbing.
    auto fieldValue = [&eval, &view](const QString &name) {
        return eval(view->page(), QStringLiteral(
            "(function(){var i=document.querySelector("
            "'input[name=%1]');return i?i.value:null;})()")
            .arg(name)).toString();
    };
    bool filled = false;
    QElapsedTimer fillTimer;
    fillTimer.start();
    while (fillTimer.elapsed() < 60000) {
        if (fieldValue(QStringLiteral("password")) == fixturePass
                && fieldValue(QStringLiteral("username")) == fixtureUser) {
            filled = true;
            break;
        }
        QTest::qWait(300);
    }
    report << QStringLiteral("username field: %1")
        .arg(fieldValue(QStringLiteral("username")));
    report << QStringLiteral("password field filled: %1")
        .arg(fieldValue(QStringLiteral("password"))
                == fixturePass ? QStringLiteral("yes")
                               : QStringLiteral("no"));
    report << QStringLiteral("execScript traffic: %1")
        .arg(eval(host->page(), QStringLiteral(
            "JSON.stringify(self.__er17exec||[])")).toString());
    report << QStringLiteral("cs log: %1")
        .arg(eval(view->page(), QStringLiteral(
            "JSON.stringify(window.__er17cs||[])"),
            agentWorld).toString());
    report << QStringLiteral("bg listener trace (base %1): %2")
        .arg(eval(host->page(), QStringLiteral(
            "String(self.__er17mlBase)")).toString())
        .arg(eval(host->page(), QStringLiteral(
            "JSON.stringify(self.__er17ml||[])")).toString());
    report << QStringLiteral("bg svc log: %1")
        .arg(eval(host->page(), QStringLiteral(
            "JSON.stringify((self.__er17svc||{}).log||[])"))
            .toString());
    report << QStringLiteral("bg mshape: %1")
        .arg(eval(host->page(), QStringLiteral(
            "(function(){var m=self.bitwardenMain;if(!m)return 'no main';"
            "return JSON.stringify({keys:Object.keys(m).filter("
            "function(k){return /autofill|cipher|log|orches|unlock/i"
            ".test(k);}),armed:!!self.__er17armed,"
            "asType:typeof m.autofillService});})()")).toString());
    report << QStringLiteral("bg autofill trace: %1")
        .arg(eval(host->page(), QStringLiteral(
            "JSON.stringify(self.__er17autofill||[])")).toString());
    report << QStringLiteral("bg logService.error: %1")
        .arg(eval(host->page(), QStringLiteral(
            "JSON.stringify(self.__er17err||[])")).toString());
    // ER18: the residual sweep needs the typed-gap list on PASS too —
    // a successful fill that still emitted an undocumented gap is a
    // failed sweep, not a quiet one.
    const QStringList hostGaps = host->shimGaps();
    report << QStringLiteral("shimGaps: %1")
        .arg(hostGaps.join(QLatin1String(" | ")));
    if (!filled) {
        // Measured-gap capture: which Bitwarden leg stopped where.
        report << QStringLiteral("gate: %1")
            .arg(eval(host->page(), QStringLiteral(
                "JSON.stringify(self.__gate||null)")).toString());
        // Did collectPageDetails / the fill message reach the page
        // agent, and did the injected autofill script register
        // listeners?
        report << QStringLiteral("agent probe: %1")
            .arg(eval(view->page(), QStringLiteral(
                "(function(){var a=window.__arktosExtAgents"
                "&&__arktosExtAgents['%1'];"
                "var om=(a&&a.chrome&&a.chrome.runtime"
                "&&a.chrome.runtime.onMessage"
                "&&a.chrome.runtime.onMessage.__count)?"
                "a.chrome.runtime.onMessage.__count():-1;"
                "return JSON.stringify({probe:a?a.probe():null,"
                "onMessageListeners:om});})()").arg(id),
                agentWorld).toString());
        report << QStringLiteral("tabs.query diag: %1")
            .arg(eval(host->page(), QStringLiteral(
                "(function(){self.__tq={done:false};"
                "chrome.tabs.query({active:true,currentWindow:true},"
                "function(t){self.__tq={done:true,tabs:t,"
                "err:chrome.runtime.lastError?"
                "chrome.runtime.lastError.message:null};});"
                "return 0;})()")).toString());
        QTest::qWait(1500);
        report << QStringLiteral("tabs.query result: %1")
            .arg(eval(host->page(), QStringLiteral(
                "JSON.stringify(self.__tq||{})")).toString());
        report << QStringLiteral("auth status now: %1")
            .arg(eval(host->page(), QStringLiteral(
                "JSON.stringify(self.__er17auth||{})")).toString());
        report << QStringLiteral("bg->page traffic: %1")
            .arg(eval(host->page(), QStringLiteral(
                "JSON.stringify(self.__er17msgs||[])")).toString());
        report << QStringLiteral("execScript traffic: %1")
            .arg(eval(host->page(), QStringLiteral(
                "JSON.stringify(self.__er17exec||[])")).toString());
        // The injected bootstrap file resolved with no per-frame
        // error yet never registered its listener — probe the
        // world's autofill-init marker, then clear it and re-eval
        // the same file inside the execSource wrapper shape so a
        // swallowed eval-time throw surfaces here verbatim.
        const QString bwFile =
            dir + QStringLiteral("/content/bootstrap-autofill-overlay.js");
        QFile bf(bwFile);
        if (bf.open(QIODevice::ReadOnly)) {
            const QString fileSrc = QString::fromUtf8(bf.readAll());
            // Plain concatenation — the bundle source must not ride
            // QString::arg (%N inside it would be rewritten).
            const QString reeval = QStringLiteral(
                "(function(){var o={};"
                "o.bw0=typeof window.bitwardenAutofillInit;"
                "o.flagCtor=window.bitwardenAutofillInit"
                "&&window.bitwardenAutofillInit.constructor"
                "&&window.bitwardenAutofillInit.constructor.name;"
                "o.flagMon=window.bitwardenAutofillInit"
                "&&window.bitwardenAutofillInit.isMonitoring;"
                "o.flagHandler=window.bitwardenAutofillInit"
                "&&typeof window.bitwardenAutofillInit"
                ".handleExtensionMessage;"
                "var a=(window.__arktosExtAgents||{})['")
                + id + QStringLiteral(
                "'];"
                "o.agentKeys=Object.keys(window.__arktosExtAgents||{});"
                "o.aId=a&&a.id;"
                "o.aExists=!!a;"
                "o.hasOwn=window.__arktosExtAgents"
                "&&Object.prototype.hasOwnProperty.call("
                "window.__arktosExtAgents,'")
                + id + QStringLiteral(
                "');"
                "o.wchrome=typeof window.chrome;"
                "o.gdoc=typeof globalThis.document;"
                "o.grs=globalThis.document"
                "?globalThis.document.readyState:'nogdoc';"
                "o.wrs=window.document"
                "?window.document.readyState:'nodoc';"
                "o.gIsW=globalThis===window;"
                "o.gAE=typeof globalThis.addEventListener;"
                "o.gloc=globalThis.location"
                "?String(globalThis.location.href):'noloc';"
                "o.cpdto=window.bitwardenAutofillInit"
                "&&window.bitwardenAutofillInit"
                ".collectPageDetailsOnLoadTimeout!==undefined"
                "?'armed':'unset';"
                "o.wrt=window.chrome&&typeof window.chrome.runtime;"
                "o.wconn=window.chrome&&window.chrome.runtime"
                "&&typeof window.chrome.runtime.connect;"
                "o.wom=window.chrome&&window.chrome.runtime"
                "&&typeof window.chrome.runtime.onMessage;"
                "o.sameChrome=a&&a.chrome?a.chrome===window.chrome:null;"
                "o.cnt0=(a&&a.chrome&&a.chrome.runtime"
                "&&a.chrome.runtime.onMessage.__count)"
                "?a.chrome.runtime.onMessage.__count():-1;"
                "try{delete window.bitwardenAutofillInit;}"
                "catch(e){}"
                "try{var __c=a&&a.chrome;"
                "(function(chrome,browser){\n")
                + fileSrc + QStringLiteral(
                "\n}).call(window,__c,__c);o.ok=1;}catch(e){"
                "o.err=String(e&&(e.stack||e.message||e));}"
                "o.bw1=typeof window.bitwardenAutofillInit;"
                "o.cnt1=(a&&a.chrome&&a.chrome.runtime"
                "&&a.chrome.runtime.onMessage.__count)"
                "?a.chrome.runtime.onMessage.__count():-1;"
                "return JSON.stringify(o);})()");
            report << QStringLiteral("world re-eval: %1")
                .arg(eval(view->page(), reeval, agentWorld).toString());
        }
    }
    report << QStringLiteral("VERDICT: %1")
        .arg(filled ? QStringLiteral(
                          "PASS — real unlock + real autofill pipeline "
                          "set input.value")
                    : QStringLiteral("MEASURED GAP — see legs above"));

    // ER19 — the earned regrade, asserted on the real bundle after a
    // proven fill: the hosted runtime serves its fatal-table
    // permissions (scripting, webRequest), so the manager row must
    // grade Limited-or-better — the remaining missingApis are the
    // genuinely unserved declares (optional nativeMessaging/privacy,
    // webRequestAuthProvider), not a condemnation.  Captured before
    // teardown while the host is still Ready.
    ExtensionManager::ExtensionInfo bwInfo;
    for (const ExtensionManager::ExtensionInfo &candidate
         : manager->extensions())
        if (candidate.id == id)
            bwInfo = candidate;
    report << QStringLiteral(
        "ER19 manager row: health=%1 hostState=%2 missingApis=%3")
        .arg(int(bwInfo.health))
        .arg(bwInfo.hostState,
             bwInfo.missingApis.join(QLatin1String(", ")));
    report << QStringLiteral("ER19 healthReason: %1")
        .arg(bwInfo.healthReason);

    for (const QString &line : report)
        qWarning().noquote() << line;
    const QString out = qEnvironmentVariable("ARKTOS_EXTTEST_OUT");
    if (!out.isEmpty()) {
        QFile file(out);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(report.join(QLatin1Char('\n')).toUtf8());
            file.write("\n");
            file.close();
        }
    }

    window->close();
    window->deleteLater();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    manager->setExtensionEnabled(id, false);
    QTRY_VERIFY_WITH_TIMEOUT(!runtime->host(id), 20000);
    manager->removeExtension(id);
    QTest::qWait(500);
    QDir(copyRoot).removeRecursively();

    QVERIFY2(filled, "the fixture credential never reached "
                     "input.value — see ER17 report legs");

    // ER19: after a proven install->unlock->fill the real bundle
    // must grade Limited-or-better — NotSupported was the pre-runtime
    // verdict, and Unknown would mean the host fold lost the liveness
    // signal entirely.
    using EHF = ExtensionManager::ExtensionHealth;
    QVERIFY2(bwInfo.health == EHF::Limited
                 || bwInfo.health == EHF::Healthy,
             qPrintable(QStringLiteral(
                 "ER19 regrade: Bitwarden still grades health=%1 "
                 "reason=\"%2\" missing=%3 — the runtime-served fatal "
                 "set must not condemn it")
                 .arg(int(bwInfo.health))
                 .arg(bwInfo.healthReason,
                      bwInfo.missingApis.join(QLatin1String(", ")))));

    // ER18 residual sweep — the deep drive must not surface any typed
    // gap outside .devin/EXT-RUNTIME-GAPS.md's documented set.
    const QStringList unplanned = undocumentedGaps(hostGaps);
    QVERIFY2(unplanned.isEmpty(),
             qPrintable(QStringLiteral(
                 "undocumented typed gaps emitted: %1 — implement the "
                 "surface or list the signature in "
                 ".devin/EXT-RUNTIME-GAPS.md")
                 .arg(unplanned.join(QLatin1String(" | ")))));
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
    // ER16: 'storage' joins the grant list — the offscreen probe
    // below round-trips chrome.storage through the document's own
    // bridge, which now enforces the manifest permission.
    const QString dir = writeExtension(
        root, QStringLiteral("tst-off"), QStringLiteral("1.0.0"),
        QString(), {QStringLiteral("offscreen"),
                    QStringLiteral("storage")});
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

// ER15 — introspection sugar.  chrome.management.getSelf answers
// the real ExtensionInfo shape from the manifest + manager records
// (ungated like Chrome); MD16 serves the declared grant's read ops
// (getAll/get + permissionWarnings) while the unserved members ride
// Chrome's failure vocabulary (no-permission undeclared, unsupported
// declared-but-unserved).  chrome.extension.getViews composes from
// the runtime's pushed context registry — the caller's own Window
// plus honest capability handles for sibling contexts (true
// documentUrl, a real location.reload(), focus honestly
// unmeasured); getBackgroundPage
// returns window in the background and the documented MV3 null
// elsewhere.  chrome.sidebarAction installs only for a manifest-
// declared sidebar_action and records panel/title while open()
// degrades typed.
void tst_ExtensionManager::extensionIntrospection()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine built without extension support");

    const QString root = QDir::temp().filePath(
        QStringLiteral("tst-extintro-%1")
            .arg(QCoreApplication::applicationPid()));
    const QString dirA = root + QLatin1String("/tst-intro-a");
    const QString dirB = root + QLatin1String("/tst-intro-b");
    for (const QString &dir : {dirA, dirB})
        QVERIFY(QDir().mkpath(dir));
    const auto writeFile = [](const QString &path,
                              const QByteArray &body) {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return false;
        f.write(body);
        f.close();
        return true;
    };
    // A: sidebar_action + action popup — the introspection fixture.
    // 'management' is deliberately NOT declared so the gate leg sees
    // no-permission, Chrome's own failure for an undeclaring caller.
    QVERIFY(writeFile(dirA + QLatin1String("/manifest.json"),
        "{\"manifest_version\":3,\"name\":\"tst-intro-a\","
        "\"version\":\"3.1.4\",\"description\":\"Intro A\","
        "\"permissions\":[\"storage\"],"
        "\"action\":{\"default_popup\":\"popup.html\"},"
        "\"sidebar_action\":{\"default_panel\":\"side.html\","
        "\"default_title\":\"Side\"},"
        "\"background\":{\"service_worker\":\"sw.js\"}}"));
    QVERIFY(writeFile(dirA + QLatin1String("/sw.js"),
        "chrome.runtime.onInstalled.addListener(function(){});\n"));
    QVERIFY(writeFile(dirA + QLatin1String("/popup.html"),
        "<!doctype html><html><body><script src=\"popup.js\">"
        "</script></body></html>"));
    QVERIFY(writeFile(dirA + QLatin1String("/popup.js"),
        "self.__boots=(self.__boots||0)+1;\n"));
    QVERIFY(writeFile(dirA + QLatin1String("/side.html"),
        "<!doctype html><title>side</title>"));
    QVERIFY(writeFile(dirA + QLatin1String("/options.html"),
        "<!doctype html><title>opt</title>"));
    // B: 'management' declared, no sidebar key — the opposite half
    // of each gate.
    QVERIFY(writeFile(dirB + QLatin1String("/manifest.json"),
        "{\"manifest_version\":3,\"name\":\"tst-intro-b\","
        "\"version\":\"9.9\",\"permissions\":[\"storage\","
        "\"management\"],"
        "\"background\":{\"service_worker\":\"sw.js\"}}"));
    QVERIFY(writeFile(dirB + QLatin1String("/sw.js"),
        "chrome.runtime.onInstalled.addListener(function(){});\n"));

    ExtensionManager *manager = ExtensionManager::instance();
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();
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
    const QString idA = loadReady(dirA, QStringLiteral("tst-intro-a"));
    const QString idB = loadReady(dirB, QStringLiteral("tst-intro-b"));
    QVERIFY2(!idA.isEmpty() && !idB.isEmpty(),
             "fixtures loaded + hosted");

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

    ExtensionBackgroundHost *hostA = runtime->host(idA);
    ExtensionBackgroundHost *hostB = runtime->host(idB);
    QVERIFY(hostA && hostB);

    // --- chrome.management.getSelf --------------------------------
    // Qt binds a native chrome.management whose getSelf answers
    // Chromium's records; the shim's identity re-assert lands on the
    // repair ticks.  Wait for OUR binding — a call in the swap
    // window would latch the native shape (no manifestVersion).
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(hostA->page(), QStringLiteral(
            "!!(chrome.management&&chrome.management.getSelf"
            "&&String(chrome.management.getSelf)"
            ".indexOf('[native code]')<0)")).toBool(),
        20000);
    hostA->page()->runJavaScript(QStringLiteral(
        "chrome.management.getSelf().then(function(v){"
        "self.__self=v;});'ok'"));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(hostA->page(), QStringLiteral(
            "!!self.__self")).toBool(), 20000);
    const QJsonObject self = evalJson(hostA->page(), QStringLiteral(
        "JSON.stringify(self.__self)"));
    QCOMPARE(self.value(QLatin1String("id")).toString(), idA);
    QCOMPARE(self.value(QLatin1String("name")).toString(),
             QStringLiteral("tst-intro-a"));
    QCOMPARE(self.value(QLatin1String("version")).toString(),
             QStringLiteral("3.1.4"));
    QCOMPARE(self.value(QLatin1String("description")).toString(),
             QStringLiteral("Intro A"));
    QCOMPARE(self.value(QLatin1String("enabled")).toBool(), true);
    QCOMPARE(self.value(QLatin1String("type")).toString(),
             QStringLiteral("extension"));
    QCOMPARE(self.value(QLatin1String("installType")).toString(),
             QStringLiteral("development"));
    QCOMPARE(self.value(QLatin1String("manifestVersion")).toInt(), 3);
    QCOMPARE(self.value(QLatin1String("mayDisable")).toBool(), true);
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(hostB->page(), QStringLiteral(
            "!!(chrome.management&&chrome.management.getSelf"
            "&&String(chrome.management.getSelf)"
            ".indexOf('[native code]')<0)")).toBool(),
        20000);
    hostB->page()->runJavaScript(QStringLiteral(
        "chrome.management.getSelf().then(function(v){"
        "self.__self=v;});'ok'"));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(hostB->page(), QStringLiteral(
            "self.__self&&self.__self.version==='9.9'"
            "&&self.__self.id==='") + idB + QLatin1String("'"))
            .toBool(),
        20000);

    // --- management failure vocabulary ----------------------------
    // A never declared 'management' -> no-permission.  B declared it
    // and MD16 serves the read ops — getAll resolves with the real
    // enumeration (B's own row included), while MD17's served
    // mutations answer their typed refusals (an unknown id is
    // not-found, never a stub).  Both ledger.
    hostA->page()->runJavaScript(QStringLiteral(
        "self.__mErr=undefined;chrome.management.getAll()"
        ".then(function(){self.__mErr='resolved';},function(e){"
        "self.__mErr=e.code||e.message;});'ok'"));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(hostA->page(), QStringLiteral(
            "self.__mErr!==undefined")).toBool(), 20000);
    QCOMPARE(eval(hostA->page(), QStringLiteral("self.__mErr"))
                 .toString(),
             QStringLiteral("no-permission"));
    hostB->page()->runJavaScript(QStringLiteral(
        "self.__mList=undefined;self.__mErr=undefined;"
        "chrome.management.getAll()"
        ".then(function(v){self.__mList=v;},function(e){"
        "self.__mErr=e.code||e.message;});'ok'"));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(hostB->page(), QStringLiteral(
            "self.__mList!==undefined||self.__mErr!==undefined"))
            .toBool(),
        20000);
    const QJsonArray mList = QJsonDocument::fromJson(
        eval(hostB->page(), QStringLiteral(
            "JSON.stringify(self.__mList||[])")).toString().toUtf8())
        .array();
    bool mSelf = false;
    for (const QJsonValue &v : mList) {
        const QJsonObject info = v.toObject();
        if (info.value(QLatin1String("id")).toString() != idB)
            continue;
        mSelf = true;
        QCOMPARE(info.value(QLatin1String("name")).toString(),
                 QStringLiteral("tst-intro-b"));
        QCOMPARE(info.value(QLatin1String("version")).toString(),
                 QStringLiteral("9.9"));
        QCOMPARE(info.value(QLatin1String("enabled")).toBool(), true);
        QVERIFY(info.value(QLatin1String("permissions")).toArray()
                .contains(QJsonValue(QStringLiteral("management"))));
    }
    QVERIFY2(mSelf, "getAll omitted the caller's own record");
    hostB->page()->runJavaScript(QStringLiteral(
        "self.__mErr=undefined;chrome.management.uninstall('x')"
        ".then(function(){self.__mErr='resolved';},function(e){"
        "self.__mErr=e.code||e.message;});'ok'"));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(hostB->page(), QStringLiteral(
            "self.__mErr!==undefined")).toBool(), 20000);
    QCOMPARE(eval(hostB->page(), QStringLiteral("self.__mErr"))
                 .toString(),
             QStringLiteral("not-found"));

    // --- extension.getBackgroundPage / getViews -------------------
    QCOMPARE(eval(hostA->page(), QStringLiteral(
        "chrome.extension.getBackgroundPage()===window")).toBool(),
             true);
    // With only the background registered, getViews sees the one
    // real window — typed filters find nothing else.
    QCOMPARE(eval(hostA->page(), QStringLiteral(
        "chrome.extension.getViews()"
        ".some(function(v){return v===window;})")).toBool(), true);
    QCOMPARE(eval(hostA->page(), QStringLiteral(
        "chrome.extension.getViews({type:'popup'}).length")).toInt(),
             0);
    QCOMPARE(eval(hostA->page(), QStringLiteral(
        "chrome.extension.getViews({type:'background'}).length"
        "===1&&chrome.extension.getViews({type:'background'})[0]"
        "===window")).toBool(), true);

    // --- extension.isAllowed*Access -------------------------------
    // MD15: the verdicts ride the bridge to invokeExtension — both
    // honestly false today (regular-profile hosting, no file:
    // grant), and crucially NOT typed stubs: a regression to the old
    // fillNs member would reject 'unsupported' and fail here.
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(hostA->page(), QStringLiteral(
            "!!(chrome.extension"
            "&&typeof chrome.extension.isAllowedIncognitoAccess"
            "==='function'"
            "&&String(chrome.extension.isAllowedIncognitoAccess)"
            ".indexOf('[native code]')<0"
            "&&typeof chrome.extension.isAllowedFileSchemeAccess"
            "==='function'"
            "&&String(chrome.extension.isAllowedFileSchemeAccess)"
            ".indexOf('[native code]')<0)")).toBool(),
        20000);
    hostA->page()->runJavaScript(QStringLiteral(
        "self.__aa=undefined;Promise.all(["
        "chrome.extension.isAllowedIncognitoAccess(),"
        "chrome.extension.isAllowedFileSchemeAccess()])"
        ".then(function(v){self.__aa='ok:'+v[0]+','+v[1];},"
        "function(e){self.__aa='err:'+(e.code||e.message);});'ok'"));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(hostA->page(), QStringLiteral("!!self.__aa")).toBool(),
        20000);
    QCOMPARE(eval(hostA->page(), QStringLiteral("self.__aa"))
                 .toString(),
             QStringLiteral("ok:false,false"));
    // The callback convention answers the same honest false.
    hostA->page()->runJavaScript(QStringLiteral(
        "self.__aacb=undefined;chrome.extension"
        ".isAllowedFileSchemeAccess(function(v){self.__aacb=v;});"
        "'ok'"));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(hostA->page(), QStringLiteral(
            "self.__aacb!==undefined")).toBool(),
        20000);
    QCOMPARE(eval(hostA->page(), QStringLiteral("self.__aacb"))
                 .toBool(),
             false);

    // --- sibling popup context ------------------------------------
    auto *popupView = new QWebEngineView;
    QWebEnginePage *popup =
        new QWebEnginePage(hostA->page()->profile(), popupView);
    popupView->setPage(popup);
    QVERIFY(runtime->attachExtensionPage(popup, idA));
    popup->load(QUrl(QStringLiteral("chrome-extension://") + idA
                     + QStringLiteral("/popup.html")));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(popup, QStringLiteral(
            "!!(window.__arktosHost&&__arktosHost.bridgeUp"
            "&&window.__arktosShim)")).toBool(),
        30000);
    // The popup's relay registration pushed the context set — the
    // host's getViews now lists a real capability handle for it.
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(hostA->page(), QStringLiteral(
            "chrome.extension.getViews({type:'popup'}).length===1"))
            .toBool(),
        20000);
    QCOMPARE(eval(hostA->page(), QStringLiteral(
        "chrome.extension.getViews({type:'popup'})[0]"
        ".__arktosViewHandle===true")).toBool(), true);
    QVERIFY(eval(hostA->page(), QStringLiteral(
        "chrome.extension.getViews({type:'popup'})[0].location.href"
        ".indexOf('/popup.html')>=0")).toBool());
    QCOMPARE(eval(hostA->page(), QStringLiteral(
        "chrome.extension.getViews().length")).toInt(), 2);
    // A handle is never the real window — and the popup sees its
    // own real window for its own type.
    QVERIFY(eval(hostA->page(), QStringLiteral(
        "chrome.extension.getViews({type:'popup'})[0]!==window"))
            .toBool());
    QCOMPARE(eval(popup, QStringLiteral(
        "chrome.extension.getViews({type:'popup'}).length===1"
        "&&chrome.extension.getViews({type:'popup'})[0]===window"))
            .toBool(), true);
    QCOMPARE(eval(popup, QStringLiteral(
        "chrome.extension.getViews({type:'background'})[0]"
        ".__arktosViewHandle===true")).toBool(), true);
    // getBackgroundPage stays the documented MV3 null outside the
    // background context — not a fake Window, not a gap.
    QCOMPARE(eval(popup, QStringLiteral(
        "chrome.extension.getBackgroundPage()===null")).toBool(),
             true);
    // A detached context has no tab — fetchProperties.tabId filters
    // it out honestly.
    QCOMPARE(eval(hostA->page(), QStringLiteral(
        "chrome.extension.getViews({type:'popup',tabId:999999})"
        ".length")).toInt(), 0);

    // The handle's one real capability: location.reload() reaches
    // the context's actual page — a real navigation, after which
    // popup.js re-ran in a fresh window (the sentinel is gone and
    // the boot counter is back at 1).
    QCOMPARE(eval(popup, QStringLiteral("self.__boots")).toInt(), 1);
    eval(popup, QStringLiteral("self.__sentinel=1;'ok'"));
    int popupLoads = 0;
    QObject::connect(popup, &QWebEnginePage::loadStarted,
                     this, [&popupLoads]() { ++popupLoads; });
    eval(hostA->page(), QStringLiteral(
        "chrome.extension.getViews({type:'popup'})[0]"
        ".location.reload();'ok'"));
    QTRY_VERIFY_WITH_TIMEOUT(popupLoads > 0, 20000);
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(popup, QStringLiteral(
            "self.__sentinel===undefined&&self.__boots===1"))
            .toBool(),
        20000);

    // --- a second, non-popup sibling (options page in a detached
    // view) registers kind 'page' -> the TAB view bucket. ----------
    auto *optView = new QWebEngineView;
    QWebEnginePage *optPage =
        new QWebEnginePage(hostA->page()->profile(), optView);
    optView->setPage(optPage);
    QVERIFY(runtime->attachExtensionPage(optPage, idA));
    optPage->load(QUrl(QStringLiteral("chrome-extension://") + idA
                       + QStringLiteral("/options.html")));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(optPage, QStringLiteral(
            "!!(window.__arktosHost&&__arktosHost.bridgeUp"
            "&&window.__arktosShim)")).toBool(),
        30000);
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(hostA->page(), QStringLiteral(
            "chrome.extension.getViews({type:'tab'}).length===1"))
            .toBool(),
        20000);
    QCOMPARE(eval(hostA->page(), QStringLiteral(
        "chrome.extension.getViews({type:'tab'})[0]"
        ".__arktosViewHandle===true")).toBool(), true);
    QCOMPARE(eval(hostA->page(), QStringLiteral(
        "chrome.extension.getViews().length")).toInt(), 3);
    // The registry itself agrees: BACKGROUND + POPUP + TAB, the
    // detached siblings carrying tabId -1.
    {
        QStringList types;
        for (const QJsonValue &c : runtime->contextsJson(idA))
            types.append(c.toObject()
                .value(QLatin1String("contextType")).toString());
        QVERIFY(types.contains(QLatin1String("BACKGROUND")));
        QVERIFY(types.contains(QLatin1String("POPUP")));
        QVERIFY(types.contains(QLatin1String("TAB")));
    }

    // Closing the popup retracts its view — the pushed registry
    // drives the count back down, nothing lingers.
    popupView->deleteLater();
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(hostA->page(), QStringLiteral(
            "chrome.extension.getViews({type:'popup'}).length===0"))
            .toBool(),
        20000);

    // --- chrome.sidebarAction --------------------------------------
    // Declared -> real bookkeeping ns; undeclared (B) -> the property
    // stays absent so feature-detect answers honestly.
    QCOMPARE(eval(hostA->page(), QStringLiteral(
        "typeof chrome.sidebarAction==='object'")).toBool(), true);
    hostA->page()->runJavaScript(QStringLiteral(
        "self.__sb={};(async function(){"
        " await chrome.sidebarAction.setPanel({panel:'side.html'});"
        " self.__sb.panel=(await chrome.sidebarAction.getPanel({}))"
        "  .panel;"
        " await chrome.sidebarAction.setTitle({title:'Side!'});"
        " self.__sb.title=(await chrome.sidebarAction.getTitle({}))"
        "  .title;"
        " self.__sb.open=await chrome.sidebarAction.isOpen({});"
        " try{await chrome.sidebarAction.open();"
        "  self.__sb.openResult='resolved';}"
        " catch(e){self.__sb.openResult=e.code||e.message;}"
        " self.__sb.done=true;"
        "})().catch(function(e){self.__sb.fatal=String(e);"
        "self.__sb.done=true;});'ok'"));
    QTRY_VERIFY_WITH_TIMEOUT(
        eval(hostA->page(), QStringLiteral("!!self.__sb.done"))
            .toBool(), 20000);
    QCOMPARE(eval(hostA->page(), QStringLiteral("self.__sb.panel"))
                 .toString(), QStringLiteral("side.html"));
    QCOMPARE(eval(hostA->page(), QStringLiteral("self.__sb.title"))
                 .toString(), QStringLiteral("Side!"));
    QCOMPARE(eval(hostA->page(), QStringLiteral("self.__sb.open"))
                 .toBool(), false);
    QCOMPARE(eval(hostA->page(), QStringLiteral(
        "self.__sb.openResult")).toString(),
             QStringLiteral("unsupported"));
    QCOMPARE(eval(hostB->page(), QStringLiteral(
        "typeof chrome.sidebarAction==='undefined'")).toBool(), true);

    // --- the health ledger saw the degradations ---------------------
    QTRY_VERIFY_WITH_TIMEOUT(
        hostA->bridge()->backend()->gaps().contains(
            QStringLiteral("chrome.management.getAll: "
                           "no-permission"))
            && hostA->bridge()->backend()->gaps().contains(
                QStringLiteral("chrome.sidebarAction.open: "
                               "unsupported")),
        20000);
    QVERIFY(hostB->bridge()->backend()->gaps().contains(
        QStringLiteral("chrome.management.uninstall: not-found")));

    // --- teardown ----------------------------------------------------
    optView->deleteLater();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    manager->setExtensionEnabled(idA, false);
    manager->setExtensionEnabled(idB, false);
    QTRY_VERIFY_WITH_TIMEOUT(
        !runtime->host(idA) && !runtime->host(idB), 20000);
    manager->removeExtension(idA);
    manager->removeExtension(idB);
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
