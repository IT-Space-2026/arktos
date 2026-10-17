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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

#include <QtTest/QtTest>
#include "qtest_arora.h"
#include "qtry.h"

#include <bookmarknode.h>
#include <bookmarksmanager.h>
#include <browserapplication.h>
#include <browsermainwindow.h>
#include <browserpaths.h>
#include <browserprofile.h>
#include <containermanager.h>
#include <cookiejar.h>
#include <historymanager.h>
#include <notesmodel.h>
#include <profileidentity.h>
#include <profileregistry.h>
#include <profilesettings.h>
#include <profilestore.h>
#include <profilevault.h>

#if defined(ARORA_RUSTCORE)
#include <sitedecisionstore.h>
#endif

#include <qdir.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qimage.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qnetworkcookie.h>
#include <qsettings.h>
#include <qsignalspy.h>
#include <qstandardpaths.h>
#include <qtcpserver.h>
#include <qtcpsocket.h>
#include <qwebenginecookiestore.h>
#include <qwebenginepage.h>
#include <qwebengineprofile.h>

// MPROF06: the multi-profile integration pass.  The unit seams each
// MPROF stage added are covered by their own suites
// (tst_profilecontext/profilesettings/profilemanagers/profilewindow/
// profilemanager) — this binary is the e2e layer the MPROF06 spec asks
// for:
//   (a) ISOLATION — two live registry personas keep history,
//       bookmarks, cookies, settings, identity and containers apart,
//       at both the app-store and the engine (cookie store) level.
//   (b) MIGRATION — a seeded pre-MPROF data dir upgrades into
//       profiles/default with byte-intact payloads + marker, and a
//       second boot is a no-op.
//   (c) BASELINE — the single-profile invariants a non-migrated user
//       must still observe (the regression sweep itself is `make
//       check`; these legs pin the visible defaults).
// The --quit-after-load-on-both-profiles leg runs against the built
// browser via `arora --quit-after-load --profile <id>` — see the
// task's done-notes.
class tst_ProfileE2E : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    // (b) first — it wipes and reseeds the layout the persona legs
    // build on.
    void migrationEndToEnd();
    void personaBringup();

    // VSEC02: the engine-storage relocation + vault-coverage legs.
    // Migration runs before any engine profile materializes; the
    // residue sweep assertion runs last of the (a) block.
    void engineStorageMigration();
    void engineStorageUnderProfileTree();

    // (a)
    void engineProfilesIsolated();
    void windowsBindProfiles();
    void cookieStoreIsolation();
    void navigationCookieHeaderProbe();
    void cookieJarRuleIsolation();
    void historyBookmarkIsolation();
    void settingsIsolation();
    void identityIsolation();
    void containersPerProfile();
    void sealedPersonaRefusesWindow();
    void engineVaultCoverage();
    void engineHoldRejectsLinks();
    void otrWritesNothing();
    void torModeLevelStore();
    void noEngineResidueAtRoot();

    // (c)
    void singleProfileBaseline();

private:
    static QString root() { return BrowserPaths::dataRoot(); }
    static QString profileDir(const QString &id)
    {
        return root() + QLatin1String("/profiles/") + id;
    }
    static QString markerPath()
    {
        return root() + QLatin1String("/migration-v1.done");
    }

    static void writeFile(const QString &path, const QByteArray &body);
    static QByteArray readFile(const QString &path);
    static void removePayload(const QString &name);
    static void resetLayout();
    static bool treeContainsUrl(BookmarkNode *node, const QString &url);

    QString m_alpha;
    QString m_beta;
};

void tst_ProfileE2E::writeFile(const QString &path, const QByteArray &body)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(path));
    QCOMPARE(file.write(body), qint64(body.size()));
}

QByteArray tst_ProfileE2E::readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    return file.readAll();
}

void tst_ProfileE2E::removePayload(const QString &name)
{
    const QString path = root() + QLatin1Char('/') + name;
    if (QFileInfo(path).isDir())
        QDir(path).removeRecursively();
    else
        QFile::remove(path);
}

// Back to a pre-MPROF01 tree: no profiles/, no marker, no registry —
// the same surgical reset tst_profilecontext uses (kept in sync: it
// lists every name kProfilePayloads can move).
void tst_ProfileE2E::resetLayout()
{
    QDir(root() + QLatin1String("/profiles")).removeRecursively();
    QFile::remove(markerPath());
    QFile::remove(root() + QLatin1String("/profiles.json"));
    static const char *names[] = {
        "history", "history.db", "history.db-wal", "history.db-shm",
        "icons",
        "bookmarks.xbel",
        "autofill.dat", "autofill-store.dat",
        "session.dat", "credentials.dat",
        "sitedecisions.json", "searchengines.json",
        "tranco-list.dat", "blocklist-domains.txt",
        "urlstrip-rules.json",
        "securestore.key", "securestore.kdf",
        "containers",
        "userscripts", "extension-updates", "extstorage",
        "notes",
        "searchengines",
        "clientcertificates",
        "servo", "pdf-view", "downloads-parts",
        "profile-avatar.png",
    };
    for (const char *name : names)
        removePayload(QLatin1String(name));
    const QStringList globbed = QDir(root()).entryList(
        QStringList() << QLatin1String("adblock_subscription_*")
                      << QLatin1String("*.migrated"),
        QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &name : globbed)
        removePayload(name);
}

bool tst_ProfileE2E::treeContainsUrl(BookmarkNode *node, const QString &url)
{
    if (!node)
        return false;
    if (node->url == url)
        return true;
    for (BookmarkNode *child : node->children()) {
        if (treeContainsUrl(child, url))
            return true;
    }
    return false;
}

void tst_ProfileE2E::initTestCase()
{
    // The application object already booted onto the migrated layout —
    // assert the baseline so a broken environment fails here rather
    // than midway through a leg.
    QVERIFY(BrowserPaths::ensureDataLayout());
    QCOMPARE(BrowserPaths::activeProfileId(), QStringLiteral("default"));
}

void tst_ProfileE2E::migrationEndToEnd()
{
    resetLayout();
    QVERIFY(!QFile::exists(markerPath()));
    QVERIFY(!QFile::exists(profileDir(QStringLiteral("default"))));

    // A seeded pre-MPROF home: profile payloads at the data root,
    // machine-global files beside them.
    const QByteArray historyBody(
        "http://migrated.example/|Migrated Page|2026-10-16T00:00:00\n");
    const QByteArray xbelBody(
        "<?xml version=\"1.0\"?>\n"
        "<xbel version=\"1.0\"><bookmark "
        "href=\"http://migrated.example/\"><title>M</title></bookmark>"
        "</xbel>\n");
    writeFile(root() + QLatin1String("/history"), historyBody);
    writeFile(root() + QLatin1String("/bookmarks.xbel"), xbelBody);
    writeFile(root() + QLatin1String("/autofill.dat"), "autofill-seed");
    writeFile(root() + QLatin1String("/profile-avatar.png"), "png-seed");
    writeFile(root() + QLatin1String("/icons/favicon.png"), "icon");
    writeFile(root() + QLatin1String("/containers/c1/sites"), "rules");
    writeFile(root() + QLatin1String("/adblock_subscription_c0ffee"),
              "sub");
    // Machine-global entries the migration must leave at the root.
    writeFile(root() + QLatin1String("/crashreport.log"), "crash");
    writeFile(root() + QLatin1String("/profiles.json"),
              "{\"version\":1,\"profiles\":[]}");

    // The settings half of a real upgrade: persona keys living in the
    // legacy single global file.
    QSettings().setValue(QLatin1String("privacy/fingerprintProtection"),
                         true);
    QSettings().setValue(QLatin1String("MainWindow/defaultEngine"),
                         QLatin1String("QtWebEngine"));
    QSettings().sync();

    QVERIFY(BrowserPaths::ensureDataLayout());
    QVERIFY(QFile::exists(markerPath()));

    // Everything a user had survives under profiles/default —
    // byte-intact and reachable through the redirected dataFilePath().
    QVERIFY(!QFile::exists(root() + QLatin1String("/history")));
    QCOMPARE(readFile(profileDir(QStringLiteral("default"))
                      + QLatin1String("/history")),
             historyBody);
    QCOMPARE(readFile(BrowserPaths::dataFilePath(
                 QLatin1String("bookmarks.xbel"))),
             xbelBody);
    QCOMPARE(readFile(BrowserPaths::dataFilePath(
                 QLatin1String("profile-avatar.png"))),
             QByteArray("png-seed"));
    QVERIFY(QFile::exists(profileDir(QStringLiteral("default"))
                          + QLatin1String("/icons/favicon.png")));
    QVERIFY(QFile::exists(profileDir(QStringLiteral("default"))
                          + QLatin1String("/containers/c1/sites")));
    QVERIFY(QFile::exists(profileDir(QStringLiteral("default"))
                          + QLatin1String(
                              "/adblock_subscription_c0ffee")));

    // Machine-global files stayed at the root.
    QVERIFY(QFile::exists(root() + QLatin1String("/crashreport.log")));
    QVERIFY(QFile::exists(root() + QLatin1String("/profiles.json")));

    // The settings split (MPROF04): classified persona keys copy into
    // profiles/default/settings.ini, the global exceptions do not, and
    // the marker makes a re-run free.
    ProfileSettings::migrateFromGlobal();
    QSettings ini(ProfileSettings::fileFor(QStringLiteral("default")),
                  QSettings::IniFormat);
    QCOMPARE(ini.value(QLatin1String("privacy/fingerprintProtection"))
                 .toBool(),
             true);
    QVERIFY(!ini.contains(QLatin1String("MainWindow/defaultEngine")));
    QCOMPARE(ini.value(QLatin1String("meta/settingsMigrated")).toBool(),
             true);
    // Copy-not-move: the legacy file keeps its keys as read fallback.
    QCOMPARE(QSettings()
                 .value(QLatin1String("privacy/fingerprintProtection"))
                 .toBool(),
             true);
    QCOMPARE(QSettings()
                 .value(QLatin1String("MainWindow/defaultEngine"))
                 .toString(),
             QStringLiteral("QtWebEngine"));
    QSettings().remove(QLatin1String("privacy/fingerprintProtection"));
    QSettings().remove(QLatin1String("MainWindow/defaultEngine"));
    QSettings().sync();

    // The registry still synthesizes its default entry over an empty
    // seeded profiles.json.
    ProfileRegistry registry(BrowserPaths::globalDataFilePath(
        QLatin1String("profiles.json")));
    QVERIFY(registry.profile(ProfileRegistry::defaultProfileId())
                .isValid());

    // Second boot is a no-op: the marker suppresses the move pass, so
    // a stray payload-named file at the root is left alone rather than
    // silently re-moved over real profile data.
    writeFile(root() + QLatin1String("/autofill.dat"),
              "stray-second-boot");
    QVERIFY(BrowserPaths::ensureDataLayout());
    QCOMPARE(readFile(root() + QLatin1String("/autofill.dat")),
             QByteArray("stray-second-boot"));
    QCOMPARE(readFile(profileDir(QStringLiteral("default"))
                      + QLatin1String("/autofill.dat")),
             QByteArray("autofill-seed"));
    QFile::remove(root() + QLatin1String("/autofill.dat"));

    // The registry survives with a real record set afterwards.
    ProfileStore *store = ProfileStore::instance();
    store->reload();
    QVERIFY(store->profile(QStringLiteral("default")).isValid());
}

void tst_ProfileE2E::personaBringup()
{
    // Two real registry personas — the same path the Profile Manager's
    // New button takes.
    ProfileStore *store = ProfileStore::instance();
    const ProfileContext alpha = store->createProfile(
        QStringLiteral("e2ealpha"));
    const ProfileContext beta = store->createProfile(
        QStringLiteral("e2ebeta"));
    QVERIFY(alpha.isValid());
    QVERIFY(beta.isValid());
    QVERIFY(alpha.id != beta.id);
    m_alpha = alpha.id;
    m_beta = beta.id;
    QVERIFY(BrowserPaths::isProfileId(m_alpha));
    QVERIFY(BrowserPaths::isProfileId(m_beta));
    QVERIFY(QFileInfo(profileDir(m_alpha)).isDir());
    QVERIFY(QFileInfo(profileDir(m_beta)).isDir());

    // Persisted, not session state: a fresh read of the registry file
    // carries both entries.
    ProfileStore fresh(store->filePath());
    QVERIFY(fresh.profile(m_alpha).isValid());
    QVERIFY(fresh.profile(m_beta).isValid());
}

void tst_ProfileE2E::engineStorageMigration()
{
    // VSEC02: the pre-change engine store at <data root>/
    // QtWebEngine/arora[-<id>] relocates into the owning persona —
    // byte-intact, never split — while legacy cache twins are dropped
    // (disposable) rather than carried.

    // Idempotent against a crashed prior run: drop any leftover
    // root-level engine tree, refusal reports and planted links
    // before seeding.  QFile::remove unlinks a symlink itself —
    // remove the root first so a planted link is never descended.
    QFile::remove(root() + QLatin1String("/QtWebEngine"));
    QDir(root() + QLatin1String("/QtWebEngine")).removeRecursively();
    QFile::remove(root() + QLatin1String("/QtWebEngine/arora-evil"));
    QFile::remove(root() + QLatin1String("/migration-v1.failed"));
    QFile::remove(root() + QLatin1String("/migration-v2.failed"));
    QFile::remove(root() + QLatin1String("/outside-target"));

    // An owned persona whose legacy engine tree holds a seeded cookie
    // db — the "session cookie survives migration" leg at file level:
    // a wholesale rename keeps every byte.
    const QString movedId = QStringLiteral("e2emoved");
    QVERIFY(QDir().mkpath(profileDir(movedId)));
    const QByteArray cookieSeed("sqlite-cookie-seed");
    writeFile(root() + QLatin1String("/QtWebEngine/arora-") + movedId
              + QLatin1String("/Network/Cookies"), cookieSeed);
    // The default persona's pre-change store is "arora" (no suffix).
    writeFile(root() + QLatin1String("/QtWebEngine/arora/Cookies"),
              "default-cookie-seed");
    // Container stores are not persona dirs — left for the container
    // code's own cleanup, never migrated by this sweep.
    writeFile(root() + QLatin1String("/QtWebEngine/")
              + QLatin1String("arora-container-zzz/x"), "x");
    // A legacy cache twin — disposable, dropped not migrated.
    const QString cacheEngineRoot =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
        + QLatin1String("/QtWebEngine");
    writeFile(cacheEngineRoot + QLatin1String("/arora-") + movedId
              + QLatin1String("/cache_data"), "cache-seed");

    QString error;
    QVERIFY2(BrowserPaths::ensureEngineStorageLayout(&error),
             qPrintable(error));

    QCOMPARE(readFile(profileDir(movedId)
                      + QLatin1String("/QtWebEngine/Network/Cookies")),
             cookieSeed);
    QCOMPARE(readFile(profileDir(QStringLiteral("default"))
                      + QLatin1String("/QtWebEngine/Cookies")),
             QByteArray("default-cookie-seed"));
    QVERIFY(!QFile::exists(root() + QLatin1String("/QtWebEngine/arora-")
                           + movedId));
    QVERIFY(!QFile::exists(root() + QLatin1String(
                               "/QtWebEngine/arora")));
    // Container-named residue is not this sweep's business.
    QVERIFY(QFile::exists(root() + QLatin1String(
                "/QtWebEngine/arora-container-zzz/x")));
    QDir(root() + QLatin1String("/QtWebEngine/arora-container-zzz"))
        .removeRecursively();
    // The cache twin is gone and nothing was carried across.
    QVERIFY(!QFile::exists(cacheEngineRoot + QLatin1String("/arora-")
                           + movedId));
    QVERIFY(!QFile::exists(profileDir(movedId)
                + QLatin1String("/cache/QtWebEngine/cache_data")));

    // Collision: a real destination means refuse — never merge, never
    // lose either side.
    const QString clashId = QStringLiteral("e2eclash");
    writeFile(profileDir(clashId) + QLatin1String("/QtWebEngine/new"),
              "new");
    writeFile(root() + QLatin1String("/QtWebEngine/arora-") + clashId
              + QLatin1String("/old"), "old");
    QVERIFY(!BrowserPaths::ensureEngineStorageLayout(&error));
    QVERIFY2(error.contains(QLatin1String("exists")),
             qPrintable(error));
    QCOMPARE(readFile(root() + QLatin1String("/QtWebEngine/arora-")
                      + clashId + QLatin1String("/old")),
             QByteArray("old"));
    QCOMPARE(readFile(profileDir(clashId)
                      + QLatin1String("/QtWebEngine/new")),
             QByteArray("new"));
    QDir(profileDir(clashId)).removeRecursively();
    QDir(root() + QLatin1String("/QtWebEngine/arora-") + clashId)
        .removeRecursively();
    QFile::remove(root() + QLatin1String("/migration-v2.failed"));

    // Symlink refusal: a planted link named like a persona store is
    // reported and left in place, never followed — the lstat rule
    // shared with the payload migration.
    writeFile(root() + QLatin1String("/outside-target"), "x");
    const QString linkPath =
        root() + QLatin1String("/QtWebEngine/arora-evil");
    QVERIFY(QFile::link(root() + QLatin1String("/outside-target"),
                        linkPath));
    QVERIFY(!BrowserPaths::ensureEngineStorageLayout(&error));
    QVERIFY2(error.contains(QLatin1String("symlink")),
             qPrintable(error));
    QVERIFY(QFileInfo(linkPath).isSymLink());
    QVERIFY(QFile::exists(root()
                          + QLatin1String("/migration-v2.failed")));
    QFile::remove(linkPath);
    QFile::remove(root() + QLatin1String("/outside-target"));
    QFile::remove(root() + QLatin1String("/migration-v2.failed"));

    // Clean pass after the refusal cases — the sweep is idempotent,
    // not marker-gated, so every boot re-verifies.
    QVERIFY2(BrowserPaths::ensureEngineStorageLayout(&error),
             qPrintable(error));
}

void tst_ProfileE2E::engineStorageUnderProfileTree()
{
    // VSEC02 (a): every named persona profile roots its engine state
    // inside the sealed tree — persistent stores under
    // profiles/<id>/QtWebEngine, the http cache under the honestly
    // excluded profiles/<id>/cache/QtWebEngine.
    QWebEngineProfile *def = BrowserProfile::normalProfile();
    // The harness's BrowserApplication ctor materialized the default
    // profile while the app name was still "Arora" — its root resolves
    // under that name, not this binary's.  Assert the seal-shape
    // invariant, which is what VSEC02 actually promises: inside
    // profiles/<id>/, never the root-level QtWebEngine/<name> dir.
    QVERIFY(def->persistentStoragePath().endsWith(
        QLatin1String("/profiles/default/QtWebEngine")));
    QVERIFY(def->cachePath().endsWith(
        QLatin1String("/profiles/default/cache/QtWebEngine")));

    for (const QString &id : {m_alpha, m_beta}) {
        QWebEngineProfile *p =
            BrowserApplication::webEngineProfileFor(id);
        QVERIFY(p);
        QCOMPARE(p->persistentStoragePath(),
                 BrowserPaths::engineStorageDir(id));
        QCOMPARE(p->cachePath(), BrowserPaths::engineCacheDir(id));
        // Inside the packed tree, not beside it — this is what makes
        // vault sealing cover Chromium's stores at all.
        QCOMPARE(p->persistentStoragePath(),
                 profileDir(id) + QLatin1String("/QtWebEngine"));
        QVERIFY(p->cachePath().startsWith(
            profileDir(id) + QLatin1String("/cache/")));
    }

    // OTR + tor stay unnamed and rootless — nothing to relocate.  Qt
    // still REPORTS a derived default (QtWebEngine/OffTheRecord) for
    // unnamed profiles; the honest disk claim is that it never
    // materializes.
    const QString otrPath =
        BrowserProfile::privateProfile()->persistentStoragePath();
    QVERIFY(otrPath.isEmpty() || !QFileInfo::exists(otrPath));
    const QString torPath =
        BrowserProfile::torProfile()->persistentStoragePath();
    QVERIFY(torPath.isEmpty() || !QFileInfo::exists(torPath));
}

void tst_ProfileE2E::engineProfilesIsolated()
{
    QWebEngineProfile *alpha =
        BrowserApplication::webEngineProfileFor(m_alpha);
    QWebEngineProfile *beta =
        BrowserApplication::webEngineProfileFor(m_beta);
    QVERIFY(alpha);
    QVERIFY(beta);
    QVERIFY(alpha != beta);
    QCOMPARE(alpha->storageName(),
             QLatin1String("arora-") + m_alpha);
    QCOMPARE(beta->storageName(),
             QLatin1String("arora-") + m_beta);
    QCOMPARE(BrowserProfile::profileIdFor(alpha), m_alpha);
    QCOMPARE(BrowserProfile::profileIdFor(beta), m_beta);

    // Chromium namespaces its own stores by storageName — separate
    // persistent roots means separate cookie db, storage, cache.
    QVERIFY(alpha->persistentStoragePath()
            != beta->persistentStoragePath());
    QVERIFY(alpha->cookieStore() != beta->cookieStore());

    // Neither aliases the default browsing profile, and repeat lookups
    // hand back the same object.
    QVERIFY(alpha != BrowserApplication::webEngineProfile());
    QCOMPARE(BrowserApplication::webEngineProfileFor(m_alpha), alpha);
}

void tst_ProfileE2E::windowsBindProfiles()
{
    // Single-tab windows only, but arm the honest "don't ask" so a
    // stray prompt can never block a close offscreen.
    for (const QString &id : {m_alpha, m_beta}) {
        ProfileSettings(id).setValue(
            QLatin1String("tabs/confirmClosingMultipleTabs"), false);
        ProfileSettings(id).setValue(
            QLatin1String("MainWindow/restoring"), false);
    }

    BrowserApplication *app = BrowserApplication::instance();
    BrowserMainWindow *windowA = app->newMainWindow(m_alpha);
    BrowserMainWindow *windowB = app->newMainWindow(m_beta);
    QVERIFY(windowA && windowB);
    QVERIFY(windowA != windowB);
    QCOMPARE(windowA->profileId(), m_alpha);
    QCOMPARE(windowB->profileId(), m_beta);
    windowA->close();
    windowB->close();
    QCoreApplication::processEvents();
    // A pushed window persona is process-global ambient state — put
    // it back or every unscoped store below resolves through beta.
    BrowserPaths::setActiveProfileId(QStringLiteral("default"));
}

void tst_ProfileE2E::cookieStoreIsolation()
{
    QWebEngineProfile *alpha =
        BrowserApplication::webEngineProfileFor(m_alpha);
    QWebEngineProfile *beta =
        BrowserApplication::webEngineProfileFor(m_beta);

    // The "logged-in on A, logged-out on B" leg at engine level: a
    // session cookie set on profile A's store fires only on that
    // store — B's store never sees it.  The store is backed by the
    // network service, which only exists once a page does — spin the
    // engine up with a real (trivial) load first.
    QWebEnginePage pageA(alpha);
    QSignalSpy loadSpy(&pageA, &QWebEnginePage::loadFinished);
    pageA.load(QUrl(QStringLiteral("about:blank")));
    QTRY_VERIFY_WITH_TIMEOUT(loadSpy.count() >= 1, 10000);

    QSignalSpy addedA(alpha->cookieStore(),
                      &QWebEngineCookieStore::cookieAdded);
    QSignalSpy addedB(beta->cookieStore(),
                      &QWebEngineCookieStore::cookieAdded);

    QNetworkCookie session(QByteArrayLiteral("sessionid"),
                           QByteArrayLiteral("alpha-secret-token"));
    session.setDomain(QLatin1String(".e2e-login.example"));
    session.setPath(QLatin1String("/"));
    alpha->cookieStore()->setCookie(
        session, QUrl(QStringLiteral("https://e2e-login.example/")));

    QTRY_VERIFY_WITH_TIMEOUT(addedA.count() >= 1, 5000);
    // A quiet window for the (never-expected) cross-store delivery.
    QTest::qWait(200);
    QCOMPARE(addedB.count(), 0);

    // The persona jars mirror their own stores for synchronous
    // callers — A's jar sees the login, B's does not.
    CookieJar *jarA = CookieJar::instanceFor(m_alpha);
    CookieJar *jarB = CookieJar::instanceFor(m_beta);
    QVERIFY(jarA != jarB);
    QCOMPARE(jarA->profile(), alpha);
    QCOMPARE(jarB->profile(), beta);
    QTRY_VERIFY_WITH_TIMEOUT(
        jarA->cookies().contains(session), 5000);
    QVERIFY(!jarB->cookies().contains(session));
    QVERIFY(jarB->cookiesForUrl(
                QUrl(QStringLiteral("https://e2e-login.example/")))
                .isEmpty());
}

void tst_ProfileE2E::navigationCookieHeaderProbe()
{
    QWebEngineProfile *alpha =
        BrowserApplication::webEngineProfileFor(m_alpha);
    QWebEngineProfile *beta =
        BrowserApplication::webEngineProfileFor(m_beta);

    // The spec's "navigation with cookie/header probes" leg: real
    // loopback HTTP endpoints read the literal request headers
    // Chromium emits on each profile's navigation.  A cookie planted
    // on A's store must ride A's request — and never B's.  Each
    // profile gets its own port so a straggler connection from A can
    // never land in B's capture (host cookies are port-blind, so the
    // probe stays honest).
    auto record = [](QTcpServer *server, QByteArrayList *requests) {
        QObject::connect(server, &QTcpServer::newConnection, server,
                         [server, requests]() {
            QTcpSocket *socket = server->nextPendingConnection();
            QObject::connect(socket, &QTcpSocket::readyRead, socket,
                             [socket, requests]() {
                requests->append(socket->readAll());
                socket->write(
                    "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                    "Connection: close\r\n\r\nok");
                socket->disconnectFromHost();
            });
        });
    };
    QTcpServer serverA;
    QTcpServer serverB;
    QVERIFY(serverA.listen(QHostAddress::LocalHost));
    QVERIFY(serverB.listen(QHostAddress::LocalHost));
    QByteArrayList requestsA;
    QByteArrayList requestsB;
    record(&serverA, &requestsA);
    record(&serverB, &requestsB);

    const QUrl urlA(QStringLiteral("http://127.0.0.1:%1/")
                        .arg(serverA.serverPort()));
    const QUrl urlB(QStringLiteral("http://127.0.0.1:%1/")
                        .arg(serverB.serverPort()));

    QNetworkCookie probe(QByteArrayLiteral("e2e-header-probe"),
                         QByteArrayLiteral("alpha-sent"));
    QSignalSpy added(alpha->cookieStore(),
                     &QWebEngineCookieStore::cookieAdded);
    alpha->cookieStore()->setCookie(probe, urlA);
    QTRY_VERIFY_WITH_TIMEOUT(added.count() >= 1, 5000);

    QWebEnginePage pageA(alpha);
    QSignalSpy loadA(&pageA, &QWebEnginePage::loadFinished);
    pageA.load(urlA);
    QTRY_VERIFY_WITH_TIMEOUT(loadA.count() >= 1, 15000);
    const QByteArray wireA = requestsA.join('\n');
    QVERIFY2(wireA.contains("GET / HTTP"), qPrintable(wireA.left(400)));
    QVERIFY2(wireA.contains("e2e-header-probe=alpha-sent"),
             qPrintable(wireA.left(400)));

    // Same host, B's profile: the request lands but carries no trace
    // of A's cookie.
    QWebEnginePage pageB(beta);
    QSignalSpy loadB(&pageB, &QWebEnginePage::loadFinished);
    pageB.load(urlB);
    QTRY_VERIFY_WITH_TIMEOUT(loadB.count() >= 1, 15000);
    const QByteArray wireB = requestsB.join('\n');
    QVERIFY2(wireB.contains("GET / HTTP"), qPrintable(wireB.left(400)));
    QVERIFY(!wireB.contains("e2e-header-probe"));
    QVERIFY(!wireB.contains("alpha-sent"));
}

void tst_ProfileE2E::cookieJarRuleIsolation()
{
    CookieJar *jarA = CookieJar::instanceFor(m_alpha);
    CookieJar *jarB = CookieJar::instanceFor(m_beta);

    // Exception lists + policies persist per persona (MPROF02): a
    // block rule under A must never steer B's filter.
    jarA->setBlockedCookies(
        QStringList() << QLatin1String("blocked-by-alpha.example"));
    QVERIFY(jarA->blockedCookies().contains(
        QLatin1String("blocked-by-alpha.example")));
    QVERIFY(!jarB->blockedCookies().contains(
        QLatin1String("blocked-by-alpha.example")));

    const CookieJar::AcceptPolicy alphaDefault = jarA->acceptPolicy();
    const CookieJar::AcceptPolicy betaDefault = jarB->acceptPolicy();
    jarA->setAcceptPolicy(CookieJar::AcceptNever);
    QCOMPARE(jarA->acceptPolicy(), CookieJar::AcceptNever);
    QCOMPARE(jarB->acceptPolicy(), betaDefault);

    // Under ARORA_RUSTCORE the exception lists persist through the
    // per-persona site-decision store — the rustcore path the
    // filter consults, not the ini file (cookiejar.cpp save()).
#if defined(ARORA_RUSTCORE)
    // The jar debounces persistence through AutoSaver (~3s) — poll
    // the per-persona decision store until the write lands.
    QString decision;
    QTRY_VERIFY_WITH_TIMEOUT(
        SiteDecisionStore::get(m_alpha, SiteDecisionStore::KindCookie,
                               QLatin1String("blocked-by-alpha.example"),
                               &decision),
        15000);
    QCOMPARE(decision, QStringLiteral("block"));
    QVERIFY(!SiteDecisionStore::get(m_beta, SiteDecisionStore::KindCookie,
                                    QLatin1String("blocked-by-alpha.example"),
                                    &decision));
    QTRY_VERIFY_WITH_TIMEOUT(
        QFile::exists(BrowserPaths::profileDataFilePath(
            m_alpha, QLatin1String("sitedecisions.json"))),
        5000);
#endif

    // Restore so other legs (and suites reusing this HOME) see the
    // default accept policy.
    jarA->setAcceptPolicy(alphaDefault);
}

void tst_ProfileE2E::historyBookmarkIsolation()
{
    HistoryManager *histA = HistoryManager::instanceFor(m_alpha);
    HistoryManager *histB = HistoryManager::instanceFor(m_beta);
    QVERIFY(histA != histB);

    const QString urlA = QStringLiteral(
        "https://e2e-alpha-history.example/only");
    const QString urlB = QStringLiteral(
        "https://e2e-beta-history.example/only");
    histA->addHistoryEntry(urlA);
    histB->addHistoryEntry(urlB);

    QVERIFY(histA->historyContains(urlA));
    QVERIFY(!histA->historyContains(urlB));
    QVERIFY(histB->historyContains(urlB));
    QVERIFY(!histB->historyContains(urlA));
    QTRY_VERIFY_WITH_TIMEOUT(
        QFile::exists(BrowserPaths::profileDataFilePath(
            m_alpha, QLatin1String("history.db")))
            || QFile::exists(BrowserPaths::profileDataFilePath(
                   m_alpha, QLatin1String("history"))),
        5000);

    BookmarksManager *marksA = BookmarksManager::instanceFor(m_alpha);
    BookmarksManager *marksB = BookmarksManager::instanceFor(m_beta);
    QVERIFY(marksA != marksB);

    const QString markUrl = QStringLiteral(
        "https://e2e-alpha-bookmark.example/only");
    QVERIFY(marksB->menu());
    BookmarkNode *node = new BookmarkNode(BookmarkNode::Bookmark);
    node->title = QStringLiteral("alpha only bookmark");
    node->url = markUrl;
    marksA->addBookmark(marksA->menu(), node);

    QVERIFY(treeContainsUrl(marksA->bookmarks(), markUrl));
    // Re-binding to beta flushes alpha's tree and shows beta's own —
    // the alpha node was never written into it.
    QVERIFY(!treeContainsUrl(marksB->bookmarks(), markUrl));
    QTRY_VERIFY_WITH_TIMEOUT(
        QFile::exists(BrowserPaths::profileDataFilePath(
            m_alpha, QLatin1String("bookmarks.xbel"))),
        5000);
}

void tst_ProfileE2E::settingsIsolation()
{
    const QLatin1String personaKey("privacy/fingerprintProtection");
    QVERIFY(ProfileSettings::isProfileKey(personaKey));

    ProfileSettings alpha(m_alpha);
    ProfileSettings beta(m_beta);
    alpha.setValue(personaKey, true);
    alpha.sync();

    QCOMPARE(alpha.value(personaKey, false).toBool(), true);
    QCOMPARE(beta.value(personaKey, false).toBool(), false);
    QCOMPARE(alpha.fileName(), ProfileSettings::fileFor(m_alpha));

    // On disk: only A's ini carries the key.
    QTRY_VERIFY_WITH_TIMEOUT(
        readFile(ProfileSettings::fileFor(m_alpha))
            .contains("fingerprintProtection"),
        5000);
    const QString betaIniPath = ProfileSettings::fileFor(m_beta);
    if (QFile::exists(betaIniPath)) {
        QSettings betaIni(betaIniPath, QSettings::IniFormat);
        QVERIFY(!betaIni.contains(personaKey));
    }

    // Global keys are shared BY DESIGN (the MPROF04 audit put
    // app-level keys on the single global file) — assert the honest
    // semantics rather than pretending they are per-profile.
    const QLatin1String globalKey("MainWindow/defaultEngine");
    QVERIFY(!ProfileSettings::isProfileKey(globalKey));
    alpha.setValue(globalKey, QLatin1String("e2e-shared"));
    QCOMPARE(beta.value(globalKey).toString(),
             QStringLiteral("e2e-shared"));
    alpha.remove(globalKey);
}

void tst_ProfileE2E::identityIsolation()
{
    ProfileStore *store = ProfileStore::instance();

    QVERIFY(store->renameProfile(m_alpha, QStringLiteral("Alice")));
    QVERIFY(store->renameProfile(m_beta, QStringLiteral("Bob")));
    store->reload();
    QCOMPARE(store->profile(m_alpha).name, QStringLiteral("Alice"));
    QCOMPARE(store->profile(m_beta).name, QStringLiteral("Bob"));

    // Avatar files live inside the owning profile's dir.
    const QString sourcePath = root()
        + QLatin1String("/e2e-avatar-source.png");
    QImage image(32, 32, QImage::Format_ARGB32);
    image.fill(QColor(32, 96, 200));
    QVERIFY(image.save(sourcePath, "PNG"));

    QVERIFY(store->setAvatarFile(m_alpha, sourcePath));
    store->reload();
    QCOMPARE(store->profile(m_alpha).avatar,
             QStringLiteral("profile-avatar.png"));
    QVERIFY(QFile::exists(store->profileFilePath(
        m_alpha, QStringLiteral("profile-avatar.png"))));
    QVERIFY(!QFile::exists(store->profileFilePath(
        m_beta, QStringLiteral("profile-avatar.png"))));
    QVERIFY(store->profile(m_beta).avatar
            != QStringLiteral("profile-avatar.png"));

    // Persisted across a fresh read, not just cached.
    ProfileStore fresh(store->filePath());
    QCOMPARE(fresh.profile(m_alpha).name, QStringLiteral("Alice"));
    QCOMPARE(fresh.profile(m_alpha).avatar,
             QStringLiteral("profile-avatar.png"));
    QCOMPARE(fresh.profile(m_beta).name, QStringLiteral("Bob"));

    QFile::remove(sourcePath);
}

void tst_ProfileE2E::containersPerProfile()
{
    ContainerManager *containersA =
        ContainerManager::instanceFor(m_alpha);
    ContainerManager *containersB =
        ContainerManager::instanceFor(m_beta);
    QVERIFY(containersA != containersB);
    QCOMPARE(containersA->profileId(), m_alpha);
    QCOMPARE(containersB->profileId(), m_beta);

    const ContainerManager::Container alphaBox =
        containersA->createContainer(QStringLiteral("Alpha Box"),
                                     QColor(Qt::darkCyan));
    QVERIFY(!alphaBox.id.isEmpty());
    QVERIFY(containersA->isContainerId(alphaBox.id));
    QVERIFY(!containersB->isContainerId(alphaBox.id));

    // The container's storage root is inside the OWNING persona's
    // dir — B's registry never learned the id.
    QVERIFY(containersA->storagePath(alphaBox.id)
            .contains(profileDir(m_alpha)));

    const ContainerManager::Container betaBox =
        containersB->createContainer(QStringLiteral("Beta Box"),
                                     QColor(Qt::darkMagenta));
    QVERIFY(!betaBox.id.isEmpty());
    QVERIFY(containersB->isContainerId(betaBox.id));
    QVERIFY(!containersA->isContainerId(betaBox.id));

    // Same local name on both personas: two independent containers —
    // the display name is free to collide, the stores never do.
    const ContainerManager::Container workA =
        containersA->createContainer(QStringLiteral("Work"),
                                     QColor(Qt::darkRed));
    const ContainerManager::Container workB =
        containersB->createContainer(QStringLiteral("Work"),
                                     QColor(Qt::darkGreen));
    QVERIFY(!workA.id.isEmpty());
    QVERIFY(!workB.id.isEmpty());
    QVERIFY(workA.id != workB.id);
    QVERIFY(containersA->isContainerId(workA.id));
    QVERIFY(containersB->isContainerId(workB.id));
    QVERIFY(!containersA->isContainerId(workB.id));
    QVERIFY(!containersB->isContainerId(workA.id));
    QVERIFY(containersA->storagePath(workA.id)
            != containersB->storagePath(workB.id));
}

void tst_ProfileE2E::sealedPersonaRefusesWindow()
{
    // VSEC01: vault.lock alone makes the persona Locked (state is
    // marker-derived, no password needed) — and the window factory
    // must fail closed on it.  A window on a sealed profile would
    // mint empty plaintext stores the next unseal then refuses to
    // overwrite.
    const QString vaultDir = ProfileVault::vaultDirFor(m_beta);
    QVERIFY(QDir().mkpath(vaultDir));
    writeFile(vaultDir + QLatin1String("/vault.lock"), "locked");
    QCOMPARE(ProfileVault::instance()->state(m_beta),
             ProfileVault::Locked);

    BrowserApplication *app = BrowserApplication::instance();
    QVERIFY(app);
    QVERIFY(!app->newMainWindow(m_beta));

    // An unsealed persona still opens normally — the gate is scoped
    // to the sealed id only.
    BrowserMainWindow *window = app->newMainWindow(m_alpha);
    QVERIFY(window);
    QCOMPARE(window->profileId(), m_alpha);
    window->close();
    QCoreApplication::processEvents();

    QDir(vaultDir).removeRecursively();
    BrowserPaths::setActiveProfileId(QStringLiteral("default"));
}

void tst_ProfileE2E::engineVaultCoverage()
{
#if !defined(ARORA_RUSTCORE)
    QSKIP("vault sealing needs a rustcore build");
#else
    if (!ProfileVault::isAvailable())
        QSKIP("rustcore vault FFI unavailable");
    ProfileVault *vault = ProfileVault::instance();

    // A dedicated persona so sealing never fights windows bound to
    // the shared e2e personas.  Clean a crashed prior run's remnants
    // so the leg is idempotent — the registry row too, or
    // createProfile suffixes the id.
    ProfileStore *store = ProfileStore::instance();
    if (store->profile(QStringLiteral("e2evault")).isValid())
        store->deleteProfile(QStringLiteral("e2evault"));
    QDir(profileDir(QStringLiteral("e2evault"))).removeRecursively();
    QDir(root() + QLatin1String("/profiles/e2evault.vault"))
        .removeRecursively();
    QDir(root() + QLatin1String("/e2evault.vault")).removeRecursively();
    QFile::remove(root() + QLatin1String(
                      "/QtWebEngine/arora-e2evault"));
    const ProfileContext ctx = store->createProfile(
        QStringLiteral("e2evault"));
    QVERIFY(ctx.isValid());
    const QString id = ctx.id;
    const QString engineDir = BrowserPaths::engineStorageDir(id);
    const QByteArray engineSeed("sealed-engine-seed");
    writeFile(engineDir + QLatin1String("/Network/Cookies"),
              engineSeed);
    // A disposable cache byte too — it must follow the exclusion,
    // not the blob.
    const QString engineCacheDir = BrowserPaths::engineCacheDir(id);
    writeFile(engineCacheDir + QLatin1String("/data_0"), "cache");

    const QByteArray pw("e2e-vault-passphrase");
    QCOMPARE(vault->seal(id, pw), ProfileVault::NoError);

    // The engine store sealed with the packed tree — nothing of it
    // remains as plaintext; the cache rode the exclusion into the
    // vault's hold instead (disposable plaintext by design).
    QVERIFY(!QFile::exists(engineDir));
    QVERIFY(!QFile::exists(engineCacheDir));
    QVERIFY(QFile::exists(ProfileVault::vaultDirFor(id)
                          + QLatin1String("/vault.lock")));
    QVERIFY(QFile::exists(ProfileVault::vaultDirFor(id)
                          + QLatin1String("/cache-hold/QtWebEngine/"
                                          "data_0")));

    // The conf records the engine boundary the sealed set covers —
    // audits read it instead of assuming.
    QFile confFile(ProfileVault::vaultDirFor(id)
                   + QLatin1String("/vault.conf"));
    QVERIFY(confFile.open(QIODevice::ReadOnly));
    const QVariantMap conf = QJsonDocument::fromJson(
        confFile.readAll()).object().toVariantMap();
    QCOMPARE(conf.value(QLatin1String("engine_storage")).toString(),
             QStringLiteral("QtWebEngine"));
    QCOMPARE(conf.value(QLatin1String("engine_cache")).toString(),
             QStringLiteral("cache/QtWebEngine"));

    // Locked-boot scenario: the sweep finds this persona's legacy
    // root-level engine dir and must park it in the vault's
    // engine-hold — never inside the (nonexistent) sealed tree.
    const QString legacy = root() + QLatin1String("/QtWebEngine/arora-")
        + id;
    writeFile(legacy + QLatin1String("/Network/Cookies"),
              "legacy-engine-seed");
    QString error;
    QVERIFY2(BrowserPaths::ensureEngineStorageLayout(&error),
             qPrintable(error));
    QVERIFY(!QFile::exists(legacy));
    QVERIFY(QFile::exists(ProfileVault::vaultDirFor(id)
                + QLatin1String("/engine-hold/QtWebEngine/"
                                "Network/Cookies")));

    // Engine residue a live profile wrote while sealed — the unseal
    // path clears it rather than deadlocking on PlaintextPresent.
    writeFile(profileDir(id)
              + QLatin1String("/QtWebEngine/residue-marker"), "x");

    QCOMPARE(vault->unseal(id, pw), ProfileVault::NoError);

    // The blob's engine tree is restored whole; the residue marker
    // was dropped, and the parked legacy store did NOT ride back —
    // the blob already carried engine state, so the hold was stale.
    QCOMPARE(readFile(engineDir + QLatin1String("/Network/Cookies")),
             engineSeed);
    QVERIFY(!QFile::exists(engineDir
                           + QLatin1String("/residue-marker")));
    QVERIFY(!QFile::exists(ProfileVault::vaultDirFor(id)
                           + QLatin1String("/engine-hold")));
    // The excluded cache came back unsealed.
    QCOMPARE(readFile(engineCacheDir + QLatin1String("/data_0")),
             QByteArray("cache"));

    // Leave the persona unlocked and clean for downstream legs.
    vault->disarmDirty(id);
#endif
}

void tst_ProfileE2E::engineHoldRejectsLinks()
{
#if !defined(ARORA_RUSTCORE)
    QSKIP("vault sealing needs a rustcore build");
#else
    // AUDIT04: a hold dir is only ever real — a symlinked
    // engine-hold/ or cache-hold/ planted while the persona is locked
    // must never move foreign content into the profile tree on unseal
    // (that would bypass the blob's MAC for the engine store), and a
    // link at the restored destination must never route Chromium's
    // writes outside it.
    if (!ProfileVault::isAvailable())
        QSKIP("rustcore vault FFI unavailable");
    ProfileVault *vault = ProfileVault::instance();

    // Dedicated persona like engineVaultCoverage — sealing refuses
    // while windows bound to the shared e2e personas are open.
    ProfileStore *store = ProfileStore::instance();
    if (store->profile(QStringLiteral("e2ehold")).isValid())
        store->deleteProfile(QStringLiteral("e2ehold"));
    QDir(profileDir(QStringLiteral("e2ehold"))).removeRecursively();
    QDir(root() + QLatin1String("/profiles/e2ehold.vault"))
        .removeRecursively();
    QDir(root() + QLatin1String("/e2ehold.vault")).removeRecursively();
    const ProfileContext ctx = store->createProfile(
        QStringLiteral("e2ehold"));
    QVERIFY(ctx.isValid());
    const QString id = ctx.id;
    const QString vaultDir = ProfileVault::vaultDirFor(id);
    const QByteArray pw("e2e-hold-link-passphrase");

    const QString engineDir = BrowserPaths::engineStorageDir(id);
    const QString liveCache =
        profileDir(id) + QLatin1String("/cache");
    writeFile(engineDir + QLatin1String("/Network/Cookies"),
              "sealed-engine-seed");
    writeFile(liveCache + QLatin1String("/seed.bin"), "cache-seed");
    QCOMPARE(vault->seal(id, pw), ProfileVault::NoError);

    // Locked: the cache rode into cache-hold (real), the engine store
    // is inside the blob.  Plant a hostile engine-hold — a symlink to
    // a foreign store — and swap the real cache-hold for a link too.
    const QString foreign = root() + QLatin1String("/foreign-store");
    writeFile(foreign + QLatin1String("/QtWebEngine/Network/Cookies"),
              "planted-foreign");
    QVERIFY(QFile::link(foreign,
                        vaultDir + QLatin1String("/engine-hold")));
    const QString foreignCache =
        root() + QLatin1String("/foreign-cache");
    writeFile(foreignCache + QLatin1String("/evil.bin"), "planted");
    QDir(vaultDir + QLatin1String("/cache-hold")).removeRecursively();
    QVERIFY(QFile::link(foreignCache,
                        vaultDir + QLatin1String("/cache-hold")));

    QCOMPARE(vault->unseal(id, pw), ProfileVault::NoError);

    // The blob's own engine store is back; the linked hold never
    // moved, and both planted links are gone (removed, not followed).
    QCOMPARE(readFile(engineDir + QLatin1String("/Network/Cookies")),
             QByteArray("sealed-engine-seed"));
    QVERIFY(!QFile::exists(vaultDir + QLatin1String("/engine-hold")));
    QVERIFY(!QFileInfo(liveCache).isSymLink());
    QVERIFY(!QFile::exists(liveCache + QLatin1String("/evil.bin")));
    // The foreign trees themselves are untouched — refusal removes
    // our links, never descends into the target.
    QVERIFY(QFile::exists(foreign
                          + QLatin1String("/QtWebEngine/Network/"
                                          "Cookies")));
    QVERIFY(QFile::exists(foreignCache + QLatin1String("/evil.bin")));

    vault->disarmDirty(id);
#endif
}

void tst_ProfileE2E::otrWritesNothing()
{
    // The shared off-the-record overlay: an unnamed in-memory engine
    // profile whose app-side stores gate on isOffTheRecord — nothing
    // a private window does can land in either persona's store.
    QWebEngineProfile *otr = BrowserProfile::privateProfile();
    QVERIFY(otr->isOffTheRecord());
    // VSEC02: the overlay reports Qt's derived default path but never
    // materializes it — nothing on disk for the relocation to miss.
    QVERIFY(otr->persistentStoragePath().isEmpty()
            || !QFileInfo::exists(otr->persistentStoragePath()));
    // Manager lookups on the overlay resolve to the mode-level
    // persona, never to a registry persona's files.
    QCOMPARE(BrowserProfile::profileIdFor(otr),
             QStringLiteral("default"));

    CookieJar *jar = CookieJar::instance(otr);
    QVERIFY(jar->isPrivate());
    NotesModel *notes = NotesModel::instance(otr);
    QVERIFY(!notes->isPersistent());
    QVERIFY(notes->storageFile().isEmpty());

    // Spin the overlay's network service up with a real page, then
    // plant a cookie: it lives on the OTR store alone — neither
    // persona jar ever mirrors it.
    QWebEnginePage page(otr);
    QSignalSpy loadSpy(&page, &QWebEnginePage::loadFinished);
    page.load(QUrl(QStringLiteral("about:blank")));
    QTRY_VERIFY_WITH_TIMEOUT(loadSpy.count() >= 1, 10000);

    QSignalSpy added(otr->cookieStore(),
                     &QWebEngineCookieStore::cookieAdded);
    QNetworkCookie secret(QByteArrayLiteral("e2e-otr"),
                          QByteArrayLiteral("must-not-persist"));
    otr->cookieStore()->setCookie(
        secret, QUrl(QStringLiteral("https://e2e-otr.example/")));
    QTRY_VERIFY_WITH_TIMEOUT(added.count() >= 1, 5000);
    QVERIFY(!CookieJar::instanceFor(m_alpha)->cookies()
                 .contains(secret));
    QVERIFY(!CookieJar::instanceFor(m_beta)->cookies()
                 .contains(secret));
}

void tst_ProfileE2E::torModeLevelStore()
{
    // Tor gets its own dedicated overlay — engine-side persistence is
    // off (unnamed + MemoryHttpCache), and its app-side stores resolve
    // to the default persona's mode-level set, exactly as documented:
    // a tor window writes only where tor already wrote pre-MPROF.
    QWebEngineProfile *tor = BrowserProfile::torProfile();
    QVERIFY(tor->isOffTheRecord());
    QVERIFY(tor != BrowserProfile::privateProfile());
    QCOMPARE(BrowserProfile::profileIdFor(tor),
             QStringLiteral("default"));
    QVERIFY(CookieJar::instance(tor)->isPrivate());
}

void tst_ProfileE2E::noEngineResidueAtRoot()
{
    // VSEC02 end-state: after every leg above materialized its
    // profiles, the data root carries no persona engine store —
    // QtWebEngine/ either does not exist or holds only the
    // container-residue dirs CONT05 already documents (Qt keys them
    // off the storage name even with overridden paths).  The sweep is
    // what keeps that true: Qt drops an empty name-keyed dir at every
    // named profile's construction, so run it once more and assert
    // only container names remain afterwards.
    QString error;
    QVERIFY2(BrowserPaths::ensureEngineStorageLayout(&error),
             qPrintable(error));
    const QString engineRoot =
        root() + QLatin1String("/QtWebEngine");
    const QFileInfoList leftovers = QDir(engineRoot).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden
        | QDir::System);
    for (const QFileInfo &left : leftovers) {
        QVERIFY2(left.fileName().startsWith(
                     QLatin1String("arora-container-")),
                 qPrintable(left.absoluteFilePath()));
    }
}

void tst_ProfileE2E::singleProfileBaseline()
{
    // (c) the zero-change invariants a single-profile user observes:
    // every unscoped resolution lands on "default", which aliases the
    // one "arora" engine profile and the profiles/default/ store.
    QCOMPARE(BrowserPaths::activeProfileId(), QStringLiteral("default"));
    QCOMPARE(ProfileSettings::currentId(), QStringLiteral("default"));
    QCOMPARE(BrowserPaths::dataFilePath(QLatin1String("history")),
             BrowserPaths::profileDataFilePath(
                 QStringLiteral("default"), QLatin1String("history")));

    QCOMPARE(BrowserApplication::webEngineProfile(),
             BrowserProfile::normalProfile());
    QCOMPARE(BrowserProfile::profileFor(QStringLiteral("default")),
             BrowserProfile::normalProfile());
    QCOMPARE(BrowserProfile::profileIdFor(
                 BrowserApplication::webEngineProfile()),
             QStringLiteral("default"));

    QCOMPARE(HistoryManager::instance(),
             HistoryManager::instanceFor(QStringLiteral("default")));
    QCOMPARE(BookmarksManager::instance(),
             BookmarksManager::instanceFor(QStringLiteral("default")));
    QCOMPARE(CookieJar::instance()->profileId(),
             QStringLiteral("default"));
    QCOMPARE(ContainerManager::instance()->profileId(),
             QStringLiteral("default"));
}

QTEST_MAIN(tst_ProfileE2E)
#include "tst_profilee2e.moc"
