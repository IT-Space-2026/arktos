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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

// RC03 probe: the rustcore profiles registry and the C++ stores must
// see each other's work through the SAME profiles.json — the
// byte-compat contract the whole migration hinges on.
//
// Under ARKTOS_RUSTCORE: a profile created through RustCoreProfiles
// (rc_profiles_*) is listed by a ProfileStore opened on the same
// file and vice versa, and the Rust-computed roots equal
// BrowserPaths' answers byte-for-byte.
//
// Under CONFIG+=no-rust: asserts the C++ ProfileStore still carries
// the registry alone — the fallback contract the flag exists for.

#include "qtest_arktos.h"

#include "browserpaths.h"
#include "profilestore.h"

#include <qfile.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qtemporarydir.h>

#if defined(ARKTOS_RUSTCORE)
#include "rustcorebridge.h"
#endif

class tst_RustProfiles : public QObject
{
    Q_OBJECT

private slots:
#if defined(ARKTOS_RUSTCORE)
    void rustCreatedVisibleToCpp();
    void cppCreatedVisibleToRust();
    void pathsMatchBrowserPaths();
    void guardsMirrorCpp();
#else
    void noRustKeepsProfileStore();
#endif
};

#if defined(ARKTOS_RUSTCORE)

void tst_RustProfiles::rustCreatedVisibleToCpp()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    RustCoreProfiles registry(dir.path());
    QVERIFY(registry.isValid());
    // A fresh root materializes the default row on first read.
    QCOMPARE(registry.profiles().size(), 1);
    QCOMPARE(registry.profiles().first().id, QLatin1String("default"));

    const ProfileContext work =
        registry.createProfile(QLatin1String("Rust Side"));
    QVERIFY(work.isValid());
    QCOMPARE(work.id, QLatin1String("rust-side"));
    QVERIFY(QFile::exists(registry.profileDir(work.id)));

    // The C++ lane opens the very file the Rust handle wrote.
    QCOMPARE(registry.filePath(), dir.filePath(QLatin1String("profiles.json")));
    ProfileStore store(registry.filePath());
    const ProfileContext seen = store.profile(work.id);
    QVERIFY(seen.isValid());
    QCOMPARE(seen.name, QLatin1String("Rust Side"));
    QCOMPARE(seen.createdAt, work.createdAt);
    // The profiles/<id>/ dir lands where the C++ store roots it.
    QCOMPARE(store.profileDir(work.id), registry.profileDir(work.id));

    // The on-disk shape is the MPROF01 document both sides own.
    QFile file(registry.filePath());
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray text = file.readAll();
    QVERIFY(text.endsWith("}\n"));
    QVERIFY(text.contains("\n    \"profiles\": [\n"));
    const QJsonObject root = QJsonDocument::fromJson(text).object();
    QCOMPARE(root.value(QLatin1String("version")).toInt(), 1);
    QCOMPARE(root.value(QLatin1String("profiles")).toArray().size(), 2);
}

void tst_RustProfiles::cppCreatedVisibleToRust()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString registryPath =
        dir.filePath(QLatin1String("profiles.json"));

    // The C++ store authors the file first.
    ProfileStore store(registryPath);
    const ProfileContext made =
        store.createProfile(QLatin1String("From Cpp"));
    QVERIFY(made.isValid());
    QCOMPARE(made.id, QLatin1String("from-cpp"));

    // Rust reads it untouched — row for row.
    RustCoreProfiles registry(dir.path());
    QVERIFY(registry.isValid());
    const ProfileContext seen = registry.profile(made.id);
    QVERIFY(seen.isValid());
    QCOMPARE(seen.name, QLatin1String("From Cpp"));
    // The file carries Qt::ISODate (no milliseconds in Qt 6.12) —
    // compare the C++ row's serialized form, not the in-memory
    // QDateTime which still holds its ms part.
    QCOMPARE(seen.createdAt,
             QDateTime::fromString(
                 made.createdAt.toString(Qt::ISODate), Qt::ISODate));
    QCOMPARE(registry.profiles().size(), store.profiles().size());

    // And a Rust-side rename reads back on the C++ side.
    QVERIFY(registry.renameProfile(made.id, QLatin1String("Renamed")));
    store.reload();
    QCOMPARE(store.profile(made.id).name, QLatin1String("Renamed"));

    // Rust delete removes the row; the C++ store agrees.
    QVERIFY(registry.deleteProfile(made.id));
    store.reload();
    QVERIFY(!store.profile(made.id).isValid());
    QVERIFY(!QFile::exists(store.profileDir(made.id)));
}

void tst_RustProfiles::pathsMatchBrowserPaths()
{
    // The default-root handle resolves roots identical to
    // BrowserPaths (test-mode QStandardPaths sandboxes dataRoot()).
    RustCoreProfiles registry;
    QVERIFY(registry.isValid());
    QCOMPARE(registry.filePath(),
             BrowserPaths::globalDataFilePath(
                 QLatin1String("profiles.json")));

    const QString id = QLatin1String("fixture_1");
    QCOMPARE(registry.profileDir(id), BrowserPaths::profileDataDir(id));
    QCOMPARE(registry.engineStorageDir(id),
             BrowserPaths::engineStorageDir(id));
    QCOMPARE(registry.engineCacheDir(id),
             BrowserPaths::engineCacheDir(id));
    QCOMPARE(registry.settingsDir(id), BrowserPaths::profileDataDir(id));
    QCOMPARE(registry.profileCacheDir(id),
             BrowserPaths::profileDataDir(id) + QLatin1String("/cache"));

    // A non-slug id resolves to default on both sides.
    QCOMPARE(registry.profileDir(QLatin1String("../escape")),
             BrowserPaths::profileDataDir(QLatin1String("../escape")));

    QCOMPARE(RustCoreProfiles::slugify(QLatin1String("Work Stuff!")),
             ProfileStore::slugify(QLatin1String("Work Stuff!")));
}

void tst_RustProfiles::guardsMirrorCpp()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    RustCoreProfiles registry(dir.path());
    QVERIFY(registry.isValid());

    // The default profile is undeletable — the full wipe stays the
    // PROF03 marker path's job.
    QVERIFY(!registry.deleteProfile(QLatin1String("default")));
    QVERIFY(!registry.deleteProfile(QLatin1String("./default")));
    QVERIFY(!registry.deleteProfile(QLatin1String("ghost")));

    // Blank display names create nothing.
    QVERIFY(!registry.createProfile(QLatin1String("   ")).isValid());

    // Traversal-shaped avatar names never reach the file.
    QVERIFY(!registry.setAvatarName(QLatin1String("default"),
                                   QLatin1String("../escape.png")));
    QVERIFY(registry.setAvatarName(QLatin1String("default"),
                                   QLatin1String("me.png")));
    QCOMPARE(registry.profile(QLatin1String("default")).avatar,
             QLatin1String("me.png"));
    QVERIFY(registry.removeAvatar(QLatin1String("default")));
    QVERIFY(registry.profile(QLatin1String("default")).avatar.isEmpty());

    // Ambient persona: process state, bogus ids resolve to default.
    QCOMPARE(registry.activeProfileId(), QLatin1String("default"));
    registry.setActiveProfileId(QLatin1String("work"));
    QCOMPARE(registry.activeProfileId(), QLatin1String("work"));
    registry.setActiveProfileId(QLatin1String("../nope"));
    QCOMPARE(registry.activeProfileId(), QLatin1String("default"));
}

#else // !ARKTOS_RUSTCORE — the fallback contract

void tst_RustProfiles::noRustKeepsProfileStore()
{
    // The no-rust build keeps ProfileStore as the registry's sole
    // owner — the rc_profiles_* surface simply is not linked in.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ProfileStore store(dir.filePath(QLatin1String("profiles.json")));
    QCOMPARE(store.profiles().size(), 1);
    const ProfileContext made =
        store.createProfile(QLatin1String("Legacy Lane"));
    QVERIFY(made.isValid());
    store.reload();
    QVERIFY(store.profile(made.id).isValid());
    QVERIFY(store.deleteProfile(made.id));
    QVERIFY(!store.profile(made.id).isValid());
}

#endif

QTEST_MAIN(tst_RustProfiles)
#include "tst_rustprofiles.moc"
