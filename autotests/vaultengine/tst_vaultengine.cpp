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

#include <profilevault.h>

#include <qdir.h>
#include <qdiriterator.h>
#include <qfile.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qsignalspy.h>
#include <qstandardpaths.h>
#include <qvariantmap.h>

/*
    PVAULT02: engine-level coverage for ProfileVault — the Qt-side
    seal/unseal orchestration over the rustcore ARV1 FFI.  These legs
    exercise the artifact layout (<id>.vault/ siblings), the marker
    state machine (lock / unlocked.dirty), the excluded-dir hold
    (cache/, profiles/), the crash-dirty scan, the overwrite-confirm
    rule, the interrupted-seal recovery, the registry record and the
    open-window lock refusal — everything tst_profilevault.cpp does
    NOT cover at the FFI level.  A dedicated non-default profile id
    keeps the fixture small: sealing "default" would pack the whole
    test data root.
*/

class tst_VaultEngine : public QObject
{
    Q_OBJECT

private slots:
    void init();

    void layout();
    void sealUnsealRoundTrip();
    void cacheAndProfilesExcluded();
    void wrongPasswordFailsClosed();
    void tamperedBlobUnpacksNothing();
    void dirtyDetection();
    void unsealRefusesExistingPlaintext();
    void sealInterruptedRecovery();
    void registryAndInspect();
    void changePasswordWhileLocked();
    void verifyOnlyChecks();
    void lockRefusesOpenWindows();
    void relockUsesCachedPassword();
    void autoLockShutdownSeals();
};

static const QLatin1String kId("pvaulttest");

static QString profileDir()
{
    return ProfileVault::profileDirFor(kId);
}

static QString vaultDir()
{
    return ProfileVault::vaultDirFor(kId);
}

static QString vaultFile(const QString &name)
{
    return vaultDir() + QLatin1Char('/') + name;
}

static bool dirHasContentsForTest(const QString &path)
{
    return !QDir(path)
        .entryList(QDir::AllEntries | QDir::NoDotAndDotDot
                   | QDir::Hidden | QDir::System)
        .isEmpty();
}

static void writeFile(const QString &path, const QByteArray &contents)
{
    QDir().mkpath(QFileInfo(path).dir().absolutePath());
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
             qPrintable(path));
    QCOMPARE(file.write(contents), qint64(contents.size()));
}

// relpath -> "f:<bytes>" / "d:" / "l:<target>" — a full tree
// fingerprint for the byte-for-byte round-trip assert.
static QVariantMap snapshot(const QString &root)
{
    QVariantMap out;
    QDir base(root);
    QDirIterator it(root,
                    QDir::AllEntries | QDir::NoDotAndDotDot
                        | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QFileInfo fi = it.fileInfo();
        const QString rel =
            base.relativeFilePath(fi.absoluteFilePath());
        if (fi.isSymLink()) {
            out.insert(rel, QLatin1String("l:")
                            + fi.symLinkTarget());
        } else if (fi.isDir()) {
            out.insert(rel + QLatin1Char('/'),
                       QLatin1String("d:"));
        } else if (fi.isFile()) {
            QFile f(fi.absoluteFilePath());
            if (f.open(QIODevice::ReadOnly))
                out.insert(rel, QLatin1String("f:")
                                + QLatin1String(f.readAll()));
        }
    }
    return out;
}

// A small but varied plaintext tree, incl. the two excluded
// top-level names and a symlink.
static void seedTree()
{
    const QString dir = profileDir();
    writeFile(dir + QLatin1String("/a/one.txt"), "alpha");
    writeFile(dir + QLatin1String("/a/sub/two.bin"),
              QByteArray("\x00\x01\x02\xfe\xff", 5));
    writeFile(dir + QLatin1String("/top.ini"), "[x]\nk=1\n");
    writeFile(dir + QLatin1String("/.hidden"), "dotfile");
    writeFile(dir + QLatin1String("/cache/blob.bin"), "cachebytes");
    writeFile(dir + QLatin1String("/profiles/sibling/note.txt"),
              "nested profile stays unsealed");
    QFile::link(QStringLiteral("a/one.txt"),
                dir + QLatin1String("/link"));
}

void tst_VaultEngine::init()
{
    ProfileVault::instance()->resetForTests();
    QDir(profileDir()).removeRecursively();
    QDir(vaultDir()).removeRecursively();
}

void tst_VaultEngine::layout()
{
    QVERIFY(ProfileVault::isAvailable());
    // MPROF01 landed first on the merged tree: "default" lives at
    // profiles/default/ like every other id.
    QCOMPARE(ProfileVault::profileDirFor(QStringLiteral("default")),
             QDir::cleanPath(
                 QStandardPaths::writableLocation(
                     QStandardPaths::AppDataLocation)
                 + QLatin1String("/profiles/default")));
    QVERIFY(profileDir().endsWith(
        QLatin1String("/profiles/pvaulttest")));
    // Artifacts are siblings of the profile dir, never inside it.
    QCOMPARE(vaultDir(),
             QFileInfo(profileDir()).dir().filePath(
                 QLatin1String("pvaulttest.vault")));
    // Non-slug ids cannot traverse out of the layout.
    QCOMPARE(ProfileVault::normalizedId(
                 QLatin1String("../escape")),
             QStringLiteral("default"));
}

void tst_VaultEngine::sealUnsealRoundTrip()
{
    ProfileVault *vault = ProfileVault::instance();
    QSignalSpy progress(vault, &ProfileVault::progress);

    seedTree();
    const QVariantMap before = snapshot(profileDir());
    QVERIFY(!before.isEmpty());

    QCOMPARE(vault->state(kId), ProfileVault::NoVault);
    QCOMPARE(vault->seal(kId, QByteArray("hunter2")),
             ProfileVault::NoError);

    // Locked: blob + markers committed, plaintext gone, registry in
    // place, exclusions held aside in the vault dir.
    QCOMPARE(vault->state(kId), ProfileVault::Locked);
    QVERIFY(QFile::exists(vaultFile(QLatin1String("vault.arv"))));
    QVERIFY(QFile::exists(vaultFile(QLatin1String("vault.lock"))));
    QVERIFY(QFile::exists(vaultFile(QLatin1String("vault.conf"))));
    QVERIFY(!QFile::exists(
        profileDir() + QLatin1String("/a/one.txt")));
    QVERIFY(progress.count() > 0);

    QCOMPARE(vault->unseal(kId, QByteArray("hunter2")),
             ProfileVault::NoError);
    // Unlocked: plaintext byte-for-byte identical, blob consumed,
    // lock marker gone, dirty marker armed.
    QCOMPARE(snapshot(profileDir()), before);
    QCOMPARE(vault->state(kId), ProfileVault::DirtyUnlocked);
    QVERIFY(!QFile::exists(vaultFile(QLatin1String("vault.arv"))));
    QVERIFY(!QFile::exists(vaultFile(QLatin1String("vault.lock"))));
    QVERIFY(QFile::exists(
        vaultFile(QLatin1String("unlocked.dirty"))));
}

void tst_VaultEngine::cacheAndProfilesExcluded()
{
    ProfileVault *vault = ProfileVault::instance();
    seedTree();
    QCOMPARE(vault->seal(kId, QByteArray("pw")), ProfileVault::NoError);
    // The excluded names never enter the archive: while locked they
    // sit unsealed in the hold, not in the tree.
    QVERIFY(!QFile::exists(profileDir() + QLatin1String("/cache")));
    QVERIFY(!QFile::exists(profileDir() + QLatin1String("/profiles")));
    QVERIFY(QFile::exists(
        vaultFile(QLatin1String("cache-hold/blob.bin"))));
    QVERIFY(QFile::exists(vaultFile(
        QLatin1String("profiles-hold/sibling/note.txt"))));
    QCOMPARE(vault->unseal(kId, QByteArray("pw")),
             ProfileVault::NoError);
    // Restored from the hold — the archive itself never carried them.
    QFile cache(profileDir() + QLatin1String("/cache/blob.bin"));
    QVERIFY(cache.open(QIODevice::ReadOnly));
    QCOMPARE(cache.readAll(), QByteArray("cachebytes"));
    QVERIFY(QFile::exists(profileDir()
        + QLatin1String("/profiles/sibling/note.txt")));
    QVERIFY(!QFile::exists(vaultFile(QLatin1String("cache-hold"))));
}

void tst_VaultEngine::wrongPasswordFailsClosed()
{
    ProfileVault *vault = ProfileVault::instance();
    seedTree();
    QCOMPARE(vault->seal(kId, QByteArray("right")),
             ProfileVault::NoError);
    QCOMPARE(vault->unseal(kId, QByteArray("wrong")),
             ProfileVault::WrongPassword);
    // Fail closed: nothing unpacked, the vault stays sealed+locked.
    QVERIFY(!dirHasContentsForTest(profileDir()));
    QCOMPARE(vault->state(kId), ProfileVault::Locked);
    QVERIFY(QFile::exists(vaultFile(QLatin1String("vault.arv"))));
    // And the real password still opens it.
    QCOMPARE(vault->unseal(kId, QByteArray("right")),
             ProfileVault::NoError);
}

void tst_VaultEngine::tamperedBlobUnpacksNothing()
{
    ProfileVault *vault = ProfileVault::instance();
    seedTree();
    QCOMPARE(vault->seal(kId, QByteArray("pw")), ProfileVault::NoError);
    // Flip a byte deep in the ciphertext payload (past the header).
    QFile blob(vaultFile(QLatin1String("vault.arv")));
    QVERIFY(blob.open(QIODevice::ReadWrite));
    QVERIFY(blob.size() > 512);
    blob.seek(blob.size() / 2);
    QByteArray byte = blob.read(1);
    byte[0] = byte.at(0) ^ 0xff;
    blob.seek(blob.size() / 2);
    QCOMPARE(blob.write(byte), qint64(1));
    blob.close();

    QCOMPARE(vault->unseal(kId, QByteArray("pw")),
             ProfileVault::Tampered);
    QVERIFY(!dirHasContentsForTest(profileDir()));
}

void tst_VaultEngine::dirtyDetection()
{
    ProfileVault *vault = ProfileVault::instance();
    QSignalSpy spy(vault, &ProfileVault::dirtyDetected);
    seedTree();
    QCOMPARE(vault->seal(kId, QByteArray("pw")), ProfileVault::NoError);
    QCOMPARE(vault->unseal(kId, QByteArray("pw")),
             ProfileVault::NoError);
    // unlocked.dirty armed + no sealed blob == died while unlocked.
    QVERIFY(vault->dirtyProfiles().contains(kId));
    QCOMPARE(vault->state(kId), ProfileVault::DirtyUnlocked);
    vault->checkForDirtyProfiles();
    QCOMPARE(spy.count(), 1);

    // A clean-shutdown acknowledgment disarms the marker.
    vault->disarmDirty(kId);
    QVERIFY(vault->dirtyProfiles().isEmpty());
    QCOMPARE(vault->state(kId), ProfileVault::Unlocked);
}

void tst_VaultEngine::unsealRefusesExistingPlaintext()
{
    ProfileVault *vault = ProfileVault::instance();
    seedTree();
    const QVariantMap before = snapshot(profileDir());
    QCOMPARE(vault->seal(kId, QByteArray("pw")), ProfileVault::NoError);
    // Something wrote into the profile dir while it was locked.
    writeFile(profileDir() + QLatin1String("/planted.txt"), "oops");

    QCOMPARE(vault->unseal(kId, QByteArray("pw")),
             ProfileVault::PlaintextPresent);
    // Explicit confirm: the parked plaintext is only destroyed after
    // the sealed tree lands intact.
    QCOMPARE(vault->unseal(kId, QByteArray("pw"), true),
             ProfileVault::NoError);
    QCOMPARE(snapshot(profileDir()), before);
    QVERIFY(!QFile::exists(vaultFile(QLatin1String("pre-unlock"))));
}

void tst_VaultEngine::sealInterruptedRecovery()
{
    ProfileVault *vault = ProfileVault::instance();
    seedTree();
    const QVariantMap before = snapshot(profileDir());
    QCOMPARE(vault->seal(kId, QByteArray("pw")), ProfileVault::NoError);
    // Crash mid-seal signature: blob committed, lock marker never
    // written, erase residue still in the dir.
    QFile::remove(vaultFile(QLatin1String("vault.lock")));
    writeFile(profileDir() + QLatin1String("/residue.txt"), "half");
    QCOMPARE(vault->state(kId), ProfileVault::SealInterrupted);

    QCOMPARE(vault->unseal(kId, QByteArray("pw"), true),
             ProfileVault::NoError);
    QCOMPARE(snapshot(profileDir()), before);
}

void tst_VaultEngine::registryAndInspect()
{
    ProfileVault *vault = ProfileVault::instance();
    seedTree();
    QCOMPARE(vault->seal(kId, QByteArray("pw"), 0x01),
             ProfileVault::NoError);

    QFile conf(vaultFile(QLatin1String("vault.conf")));
    QVERIFY(conf.open(QIODevice::ReadOnly));
    const QVariantMap registry = QJsonDocument::fromJson(
        conf.readAll()).object().toVariantMap();
    QCOMPARE(registry.value(QLatin1String("locked")).toBool(), true);
    QCOMPARE(registry.value(QLatin1String("algo")).toInt(), 1);
    QCOMPARE(registry.value(QLatin1String("auto_lock")).toBool(), true);
    QCOMPARE(registry.value(QLatin1String("profile")).toString(), kId);
    const QVariantMap kdf = registry.value(QLatin1String("kdf"))
        .toMap();
    // Honest recorded params, read back out of the sealed header.
    QCOMPARE(kdf.value(QLatin1String("memory_kib")).toInt(), 65536);
    QVERIFY(kdf.value(QLatin1String("passes")).toInt() >= 1);
    QVERIFY(registry.value(QLatin1String("excluded"))
                .toStringList()
                .contains(QLatin1String("cache")));

    const QVariantMap meta = vault->inspect(kId);
    QCOMPARE(meta.value(QLatin1String("state")).toInt(),
             int(ProfileVault::Locked));
    QVERIFY(meta.value(QLatin1String("algo_name"))
                .toString()
                .contains(QLatin1String("aes")));
    QVERIFY(meta.contains(QLatin1String("chunk_count")));
}

void tst_VaultEngine::changePasswordWhileLocked()
{
    ProfileVault *vault = ProfileVault::instance();
    seedTree();
    QCOMPARE(vault->seal(kId, QByteArray("old")),
             ProfileVault::NoError);
    QCOMPARE(vault->changePassword(kId, QByteArray("bad-old"),
                                   QByteArray("new")),
             ProfileVault::WrongPassword);
    QCOMPARE(vault->changePassword(kId, QByteArray("old"),
                                   QByteArray("new")),
             ProfileVault::NoError);
    QCOMPARE(vault->verify(kId, QByteArray("old")),
             ProfileVault::WrongPassword);
    QCOMPARE(vault->verify(kId, QByteArray("new")),
             ProfileVault::NoError);
    QCOMPARE(vault->unseal(kId, QByteArray("new")),
             ProfileVault::NoError);
}

void tst_VaultEngine::verifyOnlyChecks()
{
    ProfileVault *vault = ProfileVault::instance();
    QCOMPARE(vault->verify(kId, QByteArray("pw")),
             ProfileVault::NotLocked);
    seedTree();
    QCOMPARE(vault->seal(kId, QByteArray("pw")), ProfileVault::NoError);
    QCOMPARE(vault->verify(kId, QByteArray("pw")),
             ProfileVault::NoError);
    QCOMPARE(vault->verify(kId, QByteArray("nope")),
             ProfileVault::WrongPassword);
    // Verify never unpacks — still locked, still empty.
    QCOMPARE(vault->state(kId), ProfileVault::Locked);
    QVERIFY(!dirHasContentsForTest(profileDir()));
}

void tst_VaultEngine::lockRefusesOpenWindows()
{
    ProfileVault *vault = ProfileVault::instance();
    vault->setWindowGateForTests(true);
    seedTree();
    QCOMPARE(vault->seal(kId, QByteArray("pw")),
             ProfileVault::HasOpenWindows);
    // Nothing was touched — plaintext intact, no artifacts.
    QVERIFY(QFile::exists(profileDir() + QLatin1String("/a/one.txt")));
    QCOMPARE(vault->state(kId), ProfileVault::NoVault);
    vault->clearWindowGateForTests();
}

void tst_VaultEngine::relockUsesCachedPassword()
{
    ProfileVault *vault = ProfileVault::instance();
    seedTree();
    const QVariantMap before = snapshot(profileDir());
    QCOMPARE(vault->seal(kId, QByteArray("pw")), ProfileVault::NoError);
    QCOMPARE(vault->unseal(kId, QByteArray("pw")),
             ProfileVault::NoError);
    // The lock path (menu action, auto-lock) needs no password arg —
    // unseal cached custody for exactly this re-seal.
    QCOMPARE(vault->lockProfile(kId), ProfileVault::NoError);
    QCOMPARE(vault->state(kId), ProfileVault::Locked);
    QCOMPARE(vault->unseal(kId, QByteArray("pw")),
             ProfileVault::NoError);
    QCOMPARE(snapshot(profileDir()), before);
}

void tst_VaultEngine::autoLockShutdownSeals()
{
    ProfileVault *vault = ProfileVault::instance();
    seedTree();
    QCOMPARE(vault->seal(kId, QByteArray("pw")), ProfileVault::NoError);
    QCOMPARE(vault->unseal(kId, QByteArray("pw")),
             ProfileVault::NoError);
    QCOMPARE(vault->state(kId), ProfileVault::DirtyUnlocked);
    // The shutdown trigger re-seals auto-lock profiles without a
    // password prompt and disarms the rest.
    vault->sealUnlockedProfilesAtShutdown();
    QCOMPARE(vault->state(kId), ProfileVault::Locked);

    // auto_lock=off + clean shutdown: disarmed, left plaintext.
    QCOMPARE(vault->unseal(kId, QByteArray("pw")),
             ProfileVault::NoError);
    vault->setAutoLock(kId, false);
    vault->sealUnlockedProfilesAtShutdown();
    QCOMPARE(vault->state(kId), ProfileVault::Unlocked);
    QVERIFY(vault->dirtyProfiles().isEmpty());
    QVERIFY(QFile::exists(profileDir() + QLatin1String("/a/one.txt")));
}

QTEST_MAIN(tst_VaultEngine)
#include "tst_vaultengine.moc"
