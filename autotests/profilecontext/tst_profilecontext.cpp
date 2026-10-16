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

#include <browserpaths.h>
#include <profilecontext.h>
#include <profileidentity.h>
#include <profileregistry.h>

#include <qdir.h>
#include <qsettings.h>
#include <qtemporarydir.h>

// MPROF01: the profiles/<id>/ data-dir layout — the migration that
// moves pre-MPROF01 profile payloads into profiles/default/, the
// profiles.json registry behind it, and ProfileIdentity's delegation
// of name/avatar to the active profile's registry entry.
class tst_ProfileContext : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    void pathLayout();
    void freshBootCreatesLayout();
    void migratesSeededPayloads();
    void migrationRefusesSymlinks();
    void resumeAfterPartialMove();
    void doubleRunIsNoOp();
    void registryBasics();
    void registryIdValidation();
    void registryDropsTraversalAvatar();
    void identityDelegatesToRegistry();

private:
    static QString root() { return BrowserPaths::dataRoot(); }
    static QString profileDir()
    {
        return root() + QLatin1String("/profiles/default");
    }
    static QString markerPath()
    {
        return root() + QLatin1String("/migration-v1.done");
    }

    static void writeFile(const QString &path, const QByteArray &body);
    static void removePayload(const QString &name);
    static void resetLayout();
};

void tst_ProfileContext::writeFile(const QString &path,
                                   const QByteArray &body)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(path));
    QCOMPARE(file.write(body), qint64(body.size()));
}

void tst_ProfileContext::removePayload(const QString &name)
{
    const QString path = root() + QLatin1Char('/') + name;
    if (QFileInfo(path).isDir())
        QDir(path).removeRecursively();
    else
        QFile::remove(path);
}

void tst_ProfileContext::resetLayout()
{
    // Surgical reset — the live QtWebEngine/ store and every name the
    // migration does not own stay exactly where the running app put
    // them.
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

void tst_ProfileContext::initTestCase()
{
    QCoreApplication::setApplicationName("tst_profilecontext");
}

void tst_ProfileContext::init()
{
    resetLayout();
}

void tst_ProfileContext::pathLayout()
{
    QCOMPARE(BrowserPaths::activeProfileId(), QStringLiteral("default"));

    const QString dataPath =
        BrowserPaths::dataFilePath(QLatin1String("history"));
    QCOMPARE(dataPath, profileDir() + QLatin1String("/history"));

    QCOMPARE(BrowserPaths::profileDataFilePath(
                 QStringLiteral("work"), QStringLiteral("history")),
             root() + QLatin1String("/profiles/work/history"));

    // A non-slug id can never escape the profiles/ tree.
    QCOMPARE(BrowserPaths::profileDataDir(QStringLiteral("../etc")),
             profileDir());

    QCOMPARE(BrowserPaths::globalDataFilePath(
                 QLatin1String("crashreport.log")),
             root() + QLatin1String("/crashreport.log"));
}

void tst_ProfileContext::freshBootCreatesLayout()
{
    QVERIFY(!QFile::exists(profileDir()));
    QVERIFY(BrowserPaths::ensureDataLayout());

    QVERIFY(QFileInfo(profileDir()).isDir());
    QVERIFY(QFile::exists(markerPath()));

    // The registry materializes its default entry on first read.
    const QString registryPath = BrowserPaths::globalDataFilePath(
        QLatin1String("profiles.json"));
    ProfileRegistry registry(registryPath);
    QVERIFY(QFile::exists(registryPath));
    const ProfileContext fallback =
        registry.profile(ProfileRegistry::defaultProfileId());
    QVERIFY(fallback.isValid());
    QVERIFY(!fallback.name.isEmpty());
    QCOMPARE(fallback.avatar, QStringLiteral("profile-avatar.png"));
    QVERIFY(fallback.createdAt.isValid());
}

void tst_ProfileContext::migratesSeededPayloads()
{
    const QByteArray historyBody(
        "http://example.com/|Example|2026-10-16T00:00:00\n");
    const QByteArray xbelBody(
        "<?xml version=\"1.0\"?>\n"
        "<xbel version=\"1.0\"><bookmark "
        "href=\"http://example.com/\"><title>Ex</title></bookmark>"
        "</xbel>\n");

    static const char *files[] = {
        "history", "history.db", "history.db-wal", "history.db-shm",
        "bookmarks.xbel",
        "autofill.dat", "autofill-store.dat",
        "session.dat", "credentials.dat",
        "sitedecisions.json", "searchengines.json",
        "tranco-list.dat", "blocklist-domains.txt",
        "securestore.key", "securestore.kdf",
        "profile-avatar.png",
    };
    for (const char *name : files)
        writeFile(root() + QLatin1Char('/') + QLatin1String(name),
                  QByteArray("payload-") + name);

    static const char *dirs[] = {
        "icons", "containers", "userscripts", "extension-updates",
        "extstorage", "notes", "searchengines", "clientcertificates",
        "servo", "pdf-view", "downloads-parts",
    };
    for (const char *name : dirs)
        writeFile(root() + QLatin1Char('/') + QLatin1String(name)
                      + QLatin1String("/inner.txt"),
                  QByteArray("dir-") + name);

    writeFile(root() + QLatin1String("/adblock_subscription_ab12cd"),
              "sub-a");
    writeFile(root() + QLatin1String("/adblock_subscription_custom"),
              "sub-custom");
    // Store retirement snapshots ride along with their stores.
    writeFile(root() + QLatin1String("/searchengines.migrated/old.xml"),
              "old");

    // Real content for the survival legs.
    writeFile(root() + QLatin1String("/history"), historyBody);
    writeFile(root() + QLatin1String("/bookmarks.xbel"), xbelBody);

    // Machine-global files must NOT move.
    writeFile(root() + QLatin1String("/crashreport.log"), "crash");
    writeFile(root() + QLatin1String("/tor/daemon-state"), "tor");
    writeFile(root() + QLatin1String("/locale/en.qm"), "qm");
    writeFile(root() + QLatin1String("/arora-profile-wipe.pending"),
              "wipe");
    writeFile(root() + QLatin1String("/profiles.json"),
              "{\"version\":1,\"profiles\":[]}");

    QVERIFY(BrowserPaths::ensureDataLayout());

    // Every payload landed under profiles/default with its bytes.
    for (const char *name : files) {
        const QString n = QLatin1String(name);
        QVERIFY2(!QFile::exists(root() + QLatin1Char('/') + n),
                 qPrintable(n));
        QVERIFY2(QFile::exists(profileDir() + QLatin1Char('/') + n),
                 qPrintable(n));
    }
    for (const char *name : dirs) {
        const QString n = QLatin1String(name);
        QVERIFY2(QFile::exists(profileDir() + QLatin1Char('/') + n
                               + QLatin1String("/inner.txt")),
                 qPrintable(n));
        QVERIFY2(!QFile::exists(root() + QLatin1Char('/') + n),
                 qPrintable(n));
    }
    QVERIFY(QFile::exists(profileDir()
            + QLatin1String("/adblock_subscription_ab12cd")));
    QVERIFY(QFile::exists(profileDir()
            + QLatin1String("/adblock_subscription_custom")));
    QVERIFY(QFile::exists(profileDir()
            + QLatin1String("/searchengines.migrated/old.xml")));

    // History and bookmarks survived the move byte-for-byte — and are
    // reachable through the redirected dataFilePath().
    QFile history(BrowserPaths::dataFilePath(QLatin1String("history")));
    QVERIFY(history.open(QIODevice::ReadOnly));
    QCOMPARE(history.readAll(), historyBody);
    QFile xbel(
        BrowserPaths::dataFilePath(QLatin1String("bookmarks.xbel")));
    QVERIFY(xbel.open(QIODevice::ReadOnly));
    QCOMPARE(xbel.readAll(), xbelBody);

    // Globals stayed at the root.
    for (const char *name :
         {"crashreport.log", "arora-profile-wipe.pending",
          "profiles.json"}) {
        QVERIFY2(QFile::exists(root() + QLatin1Char('/')
                               + QLatin1String(name)),
                 name);
    }
    QVERIFY(QFile::exists(root() + QLatin1String("/tor/daemon-state")));
    QVERIFY(QFile::exists(root() + QLatin1String("/locale/en.qm")));
    QVERIFY(QFile::exists(markerPath()));
}

void tst_ProfileContext::migrationRefusesSymlinks()
{
    // VSEC01: a planted link must never ride into profiles/default —
    // QFile::rename moves the LINK, and every later open() resolves
    // to the attacker's target.  The pass must fail closed.
#ifndef Q_OS_UNIX
    QSKIP("symlink migration legs are unix-only");
#endif
    // A payload-named link: exists() follows it, so without the guard
    // the pass would happily carry it.
    writeFile(root() + QLatin1String("/escape-target"), "planted");
    QVERIFY(QFile::link(root() + QLatin1String("/escape-target"),
                        root() + QLatin1String("/history")));
    writeFile(root() + QLatin1String("/bookmarks.xbel"), "b");

    QString error;
    QVERIFY2(!BrowserPaths::ensureDataLayout(&error),
             qPrintable(error));
    // The link was NOT moved into the profile tree and the real
    // payloads stayed put — nothing is migrated under a hostile dir.
    QVERIFY(!QFile::exists(profileDir() + QLatin1String("/history")));
    QVERIFY(QFileInfo(root() + QLatin1String("/history")).isSymLink());
    QVERIFY(QFile::exists(root() + QLatin1String("/bookmarks.xbel")));
    QVERIFY(!QFile::exists(markerPath()));

    QFile::remove(root() + QLatin1String("/history"));
    QFile::remove(root() + QLatin1String("/escape-target"));
    QFile::remove(root() + QLatin1String("/bookmarks.xbel"));

    // A link AS the profiles container is refused before mkpath or
    // any rename could follow it out of the data root.  Leg 1's
    // failed pass still created the real profiles/ dir — clear it so
    // the link can stand in its place.
    QDir(root() + QLatin1String("/profiles")).removeRecursively();
    QTemporaryDir outside;
    QVERIFY(outside.isValid());
    QVERIFY(QFile::link(outside.path(),
                        root() + QLatin1String("/profiles")));
    writeFile(root() + QLatin1String("/history"), "h");
    QVERIFY2(!BrowserPaths::ensureDataLayout(&error),
             qPrintable(error));
    QVERIFY(QFileInfo(root() + QLatin1String("/profiles")).isSymLink());
    QVERIFY(!QFile::exists(outside.filePath(
        QLatin1String("default/history"))));
    QVERIFY(QFile::exists(root() + QLatin1String("/history")));
    QFile::remove(root() + QLatin1String("/profiles"));
}

void tst_ProfileContext::resumeAfterPartialMove()
{
    // A crashed first boot: one payload moved by hand, no marker —
    // the migration must resume and finish, not split state.
    writeFile(root() + QLatin1String("/history"), "h");
    writeFile(root() + QLatin1String("/bookmarks.xbel"), "b");
    writeFile(root() + QLatin1String("/autofill.dat"), "a");
    QDir().mkpath(profileDir());
    QVERIFY(QFile::rename(root() + QLatin1String("/history"),
                          profileDir() + QLatin1String("/history")));

    QVERIFY(BrowserPaths::ensureDataLayout());
    QVERIFY(QFile::exists(profileDir() + QLatin1String("/history")));
    QVERIFY(QFile::exists(profileDir() + QLatin1String("/bookmarks.xbel")));
    QVERIFY(QFile::exists(profileDir() + QLatin1String("/autofill.dat")));
    QVERIFY(QFile::exists(markerPath()));
}

void tst_ProfileContext::doubleRunIsNoOp()
{
    writeFile(root() + QLatin1String("/history"), "h");
    QVERIFY(BrowserPaths::ensureDataLayout());
    QVERIFY(QFile::exists(markerPath()));

    // Once the marker exists the pass is finished — a stray
    // payload-named file at the root is left alone rather than
    // silently re-moved over real profile data.
    writeFile(root() + QLatin1String("/autofill.dat"), "stray");
    QVERIFY(BrowserPaths::ensureDataLayout());
    QVERIFY(QFile::exists(root() + QLatin1String("/autofill.dat")));
    QVERIFY(!QFile::exists(profileDir() + QLatin1String("/autofill.dat")));
}

void tst_ProfileContext::registryBasics()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString filePath = dir.filePath(QLatin1String("profiles.json"));

    // Missing file -> default synthesized and persisted.
    {
        ProfileRegistry registry(filePath);
        QCOMPARE(registry.profiles().size(), 1);
        const ProfileContext fallback =
            registry.profile(QStringLiteral("default"));
        QVERIFY(fallback.isValid());
        QVERIFY(QFile::exists(filePath));
    }

    // updateProfile persists across instances and emits the signal.
    {
        ProfileRegistry registry(filePath);
        QSignalSpy spy(&registry, &ProfileRegistry::profileChanged);
        ProfileContext context =
            registry.profile(QStringLiteral("default"));
        context.name = QStringLiteral("Work");
        QVERIFY(registry.updateProfile(context));
        QCOMPARE(spy.count(), 1);
        // Unknown ids are refused — the id is immutable.
        QVERIFY(!registry.updateProfile(
            ProfileContext(QStringLiteral("ghost"),
                           QStringLiteral("Ghost"))));
        QVERIFY(!registry.updateProfile(
            ProfileContext(QStringLiteral("../escape"),
                           QStringLiteral("Bad"))));
    }
    {
        ProfileRegistry registry(filePath);
        QCOMPARE(registry.profile(QStringLiteral("default")).name,
                 QStringLiteral("Work"));
        QVERIFY(!registry.profile(QStringLiteral("ghost")).isValid());
    }

    // A corrupt registry still yields the default profile but is not
    // clobbered behind the user's back.
    writeFile(filePath, "{not json");
    {
        ProfileRegistry registry(filePath);
        QVERIFY(registry.profile(QStringLiteral("default")).isValid());
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("{not json"));
    }
}

void tst_ProfileContext::registryIdValidation()
{
    QVERIFY(ProfileRegistry::isValidProfileId(QStringLiteral("default")));
    QVERIFY(ProfileRegistry::isValidProfileId(QStringLiteral("work-2")));
    QVERIFY(ProfileRegistry::isValidProfileId(QStringLiteral("a_b")));
    QVERIFY(!ProfileRegistry::isValidProfileId(QString()));
    QVERIFY(!ProfileRegistry::isValidProfileId(QStringLiteral("..")));
    QVERIFY(!ProfileRegistry::isValidProfileId(QStringLiteral("a/b")));
    QVERIFY(!ProfileRegistry::isValidProfileId(QStringLiteral("A")));
    QCOMPARE(ProfileRegistry::slugify(QStringLiteral("Work Stuff")),
             QStringLiteral("work-stuff"));
    QCOMPARE(ProfileRegistry::slugify(QStringLiteral("  ")),
             QStringLiteral("profile"));
}

void tst_ProfileContext::registryDropsTraversalAvatar()
{
    // VSEC01: the avatar field is joined under profiles/<id>/ — a
    // crafted registry value must not survive load or update as a
    // path that escapes the profile dir.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString filePath = dir.filePath(QLatin1String("profiles.json"));

    // Load-time: a traversal-shaped avatar drops to empty.
    writeFile(filePath,
        "{\"version\":1,\"profiles\":[{\"id\":\"work\",\"name\":\"W\","
        "\"avatar\":\"../../outside.png\",\"createdAt\":"
        "\"2026-01-01T00:00:00Z\"}]}");
    {
        ProfileRegistry registry(filePath);
        const ProfileContext work =
            registry.profile(QStringLiteral("work"));
        QVERIFY(work.isValid());
        QVERIFY2(work.avatar.isEmpty(), qPrintable(work.avatar));
    }

    // Update-time: a caller handing a traversal value gets it
    // stripped before it can reach disk.
    {
        ProfileRegistry registry(filePath);
        ProfileContext work = registry.profile(QStringLiteral("work"));
        work.avatar = QStringLiteral("../evil.png");
        QVERIFY(registry.updateProfile(work));
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY2(!file.readAll().contains("evil.png"),
                 "traversal avatar must never reach the file");
    }
    {
        ProfileRegistry registry(filePath);
        QVERIFY(registry.profile(QStringLiteral("work"))
                    .avatar.isEmpty());
    }

    // A plain basename survives both directions.
    {
        ProfileRegistry registry(filePath);
        ProfileContext work = registry.profile(QStringLiteral("work"));
        work.avatar = QStringLiteral("me.png");
        QVERIFY(registry.updateProfile(work));
    }
    {
        ProfileRegistry registry(filePath);
        QCOMPARE(registry.profile(QStringLiteral("work")).avatar,
                 QStringLiteral("me.png"));
    }
}

void tst_ProfileContext::identityDelegatesToRegistry()
{
    QVERIFY(BrowserPaths::ensureDataLayout());

    // The legacy PROF02 QSettings key imports once, when the default
    // entry is first synthesized.
    QSettings().setValue(QLatin1String("profile/name"),
                         QLatin1String("Legacy Ada"));
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        ProfileRegistry imported(
            dir.filePath(QLatin1String("profiles.json")));
        QCOMPARE(imported.profile(QStringLiteral("default")).name,
                 QStringLiteral("Legacy Ada"));
    }
    QSettings().remove(QLatin1String("profile/name"));

    // ProfileIdentity reads/writes the registry — its public API is
    // unchanged, the QSettings key is not.
    ProfileRegistry *registry = ProfileRegistry::instance();
    ProfileIdentity *identity = ProfileIdentity::instance();
    QSignalSpy spy(identity, &ProfileIdentity::nameChanged);

    identity->setName(QStringLiteral("  Work  "));
    QCOMPARE(identity->name(), QStringLiteral("Work"));
    QCOMPARE(registry->activeProfile().name, QStringLiteral("Work"));
    QCOMPARE(spy.count(), 1);
    QVERIFY(QSettings().value(QLatin1String("profile/name")).isNull());

    // External registry writes repaint through the relay.
    ProfileContext context = registry->activeProfile();
    context.name = QStringLiteral("External");
    QVERIFY(registry->updateProfile(context));
    QCOMPARE(identity->name(), QStringLiteral("External"));
    QCOMPARE(spy.count(), 2);

    // The avatar path lives inside the active profile's dir.
    QVERIFY(identity->avatarFilePath().startsWith(profileDir()));
    QVERIFY(identity->avatarFilePath().endsWith(
        QLatin1String("profile-avatar.png")));

    // Restore the default so other suites see a neutral identity.
    identity->setName(QStringLiteral("Profile"));
}

QTEST_MAIN(tst_ProfileContext)
#include "tst_profilecontext.moc"
