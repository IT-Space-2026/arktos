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
#include <qelapsedtimer.h>
#include <qfile.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qsignalspy.h>
#include <qvariantmap.h>

/*
    PVAULT04: end-to-end pipeline coverage for the profile vault —
    the full lock/unlock lifecycle as one ordered scenario rather
    than the per-mechanism legs tst_vaultengine runs.  The spec's UI
    steps (the unlock prompt, the dirty-boot "Re-seal now" offer, the
    profile-menu Lock item, Delete Profile) are PVAULT03's dialogs on
    the UI lane; every one of them drives exactly the ProfileVault
    calls exercised here, so this suite is the merge seam's contract
    test: unseal() is what the prompt calls, seal()/lockProfile() is
    what the accepted offer and the menu item call, wipeVault() is
    what Delete Profile calls.

    Legs: create profile -> set password -> lock -> dir replaced by
    sealed blob + marker -> unlock -> plaintext byte-for-byte ->
    auto-lock on shutdown -> tamper refuses with zero unpack ->
    crash-dirty boot fires the offer -> re-seal works -> N wrong
    passwords cost honest argon2 time (no lockout theater) -> locked
    profile yields no plaintext reads -> Delete Profile wipes the
    vault without the password.
*/

class tst_VaultE2E : public QObject
{
    Q_OBJECT

private slots:
    void init();

    void pipelineRoundTrip();
    void lockedProfileYieldsNoPlaintextReads();
    void wrongPasswordAttemptsCostKdfTime();
    void tamperedCiphertextRefuses();
    void dirtyCrashResealOffer();
    void deleteLockedProfileWipesVault();
    void bootLeavesLockedProfileSealed();
};

static const QLatin1String kId("vaulte2e");

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

static void writeFile(const QString &path, const QByteArray &contents)
{
    QDir().mkpath(QFileInfo(path).dir().absolutePath());
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
             qPrintable(path));
    QCOMPARE(file.write(contents), qint64(contents.size()));
}

// relpath -> "f:<bytes>" / "d:" / "l:<target>" — full tree
// fingerprint for the byte-for-byte asserts.
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

// The profile tree as the stores actually lay it out — named after
// the real artifacts so the "locked profile yields no reads" leg is
// honest about what is being sealed away.
static void seedProfileTree()
{
    const QString dir = profileDir();
    writeFile(dir + QLatin1String("/history.db"),
              "SQLITE-CANARY history rows live here");
    writeFile(dir + QLatin1String("/cookies"),
              "COOKIE-CANARY session cookies live here");
    writeFile(dir + QLatin1String("/settings.ini"),
              "[MainWindow]\nSECRET-CANARY=1\n");
    writeFile(dir + QLatin1String("/bookmarks.xbel"),
              "<xbel><bookmark href=\"XBEL-CANARY\"/></xbel>");
    writeFile(dir + QLatin1String("/cache/blob.bin"), "cachebytes");
}

void tst_VaultE2E::init()
{
    ProfileVault::instance()->resetForTests();
    QDir(profileDir()).removeRecursively();
    QDir(vaultDir()).removeRecursively();
}

// The spec's main pipeline, in order: create -> password -> lock ->
// sealed artifacts replace the dir -> unlock (the prompt's call) ->
// byte-identical plaintext -> auto-lock on shutdown -> unlock again.
void tst_VaultE2E::pipelineRoundTrip()
{
    ProfileVault *vault = ProfileVault::instance();

    // create profile
    seedProfileTree();
    const QVariantMap before = snapshot(profileDir());
    QVERIFY(!before.isEmpty());

    // windows open on the profile -> lock refused, nothing touched
    vault->setWindowGateForTests(true);
    QCOMPARE(vault->seal(kId, QByteArray("correct horse")),
             ProfileVault::HasOpenWindows);
    QCOMPARE(vault->state(kId), ProfileVault::NoVault);
    vault->clearWindowGateForTests();

    // set password -> lock: the dir is replaced by blob + marker
    QCOMPARE(vault->seal(kId, QByteArray("correct horse")),
             ProfileVault::NoError);
    QCOMPARE(vault->state(kId), ProfileVault::Locked);
    QVERIFY(QFile::exists(vaultFile(QLatin1String("vault.arv"))));
    QVERIFY(QFile::exists(vaultFile(QLatin1String("vault.lock"))));
    QVERIFY(QFile::exists(vaultFile(QLatin1String("vault.conf"))));
    QVERIFY(!QFile::exists(
        profileDir() + QLatin1String("/history.db")));

    // unlock via the prompt path -> plaintext byte-for-byte
    QCOMPARE(vault->unseal(kId, QByteArray("correct horse")),
             ProfileVault::NoError);
    QCOMPARE(snapshot(profileDir()), before);
    QCOMPARE(vault->state(kId), ProfileVault::DirtyUnlocked);

    // last-window-close = app shutdown pre-MPROF03: the auto-lock
    // re-seals with the custody password, no prompt needed
    vault->sealUnlockedProfilesAtShutdown();
    QCOMPARE(vault->state(kId), ProfileVault::Locked);
    QVERIFY(QFile::exists(vaultFile(QLatin1String("vault.arv"))));

    // and the vault still opens with the same password
    QCOMPARE(vault->unseal(kId, QByteArray("correct horse")),
             ProfileVault::NoError);
    QCOMPARE(snapshot(profileDir()), before);
}

// A locked profile never yields history/cookies reads: the store
// files are inside the ciphertext and nothing readable remains.
void tst_VaultE2E::lockedProfileYieldsNoPlaintextReads()
{
    ProfileVault *vault = ProfileVault::instance();
    seedProfileTree();
    QCOMPARE(vault->seal(kId, QByteArray("pw")), ProfileVault::NoError);

    // Nothing to read: the profile dir holds zero entries.
    QVERIFY(QDir(profileDir())
                .entryList(QDir::AllEntries | QDir::NoDotAndDotDot
                           | QDir::Hidden | QDir::System)
                .isEmpty());

    // The canaries are inside the sealed blob only — and the blob is
    // ciphertext: no canary byte string appears in it.
    QFile blob(vaultFile(QLatin1String("vault.arv")));
    QVERIFY(blob.open(QIODevice::ReadOnly));
    const QByteArray sealed = blob.readAll();
    QVERIFY(!sealed.isEmpty());
    QVERIFY(!sealed.contains("SQLITE-CANARY"));
    QVERIFY(!sealed.contains("COOKIE-CANARY"));
    QVERIFY(!sealed.contains("SECRET-CANARY"));
    QVERIFY(!sealed.contains("XBEL-CANARY"));

    // verify() without the password cannot even open the blob.
    QCOMPARE(vault->verify(kId, QByteArray("guessed")),
             ProfileVault::WrongPassword);
}

// Wrong-password N attempts: each costs real argon2 memory-hardness
// time, each fails closed, there is no lockout counter anywhere —
// and the correct password still works afterwards.
void tst_VaultE2E::wrongPasswordAttemptsCostKdfTime()
{
    ProfileVault *vault = ProfileVault::instance();
    seedProfileTree();
    QCOMPARE(vault->seal(kId, QByteArray("right")),
             ProfileVault::NoError);
    const QByteArray confBefore = [&]() {
        QFile f(vaultFile(QLatin1String("vault.conf")));
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }();
    QVERIFY(!confBefore.isEmpty());

    const QStringList vaultFilesBefore = QDir(vaultDir()).entryList(
        QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden);

    qint64 slowest = 0;
    for (int i = 0; i < 3; ++i) {
        QElapsedTimer timer;
        timer.start();
        QCOMPARE(vault->unseal(kId, QByteArray("wrong-")
                                        .append(QByteArray::number(i))),
                 ProfileVault::WrongPassword);
        const qint64 elapsed = timer.elapsed();
        // The floor proves the attempt ran the memory-hard KDF — a
        // compare-only shortcut would return in microseconds.
        QVERIFY2(elapsed >= 15,
                 qPrintable(QStringLiteral(
                     "attempt %1 returned in %2ms — no KDF cost")
                                .arg(i).arg(elapsed)));
        slowest = qMax(slowest, elapsed);
        QCOMPARE(vault->state(kId), ProfileVault::Locked);
        QVERIFY(QDir(profileDir())
                    .entryList(QDir::AllEntries | QDir::NoDotAndDotDot
                               | QDir::Hidden | QDir::System)
                    .isEmpty());
    }
    qInfo() << "wrong-password attempts: slowest" << slowest << "ms";

    // No lockout theater: the registry is untouched and no backoff
    // marker or counter file appeared in the vault dir.
    QFile conf(vaultFile(QLatin1String("vault.conf")));
    QVERIFY(conf.open(QIODevice::ReadOnly));
    QCOMPARE(conf.readAll(), confBefore);
    QCOMPARE(QDir(vaultDir()).entryList(
                 QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden),
             vaultFilesBefore);

    // The correct password still opens it — failures cost time,
    // not access.
    QCOMPARE(vault->unseal(kId, QByteArray("right")),
             ProfileVault::NoError);
}

// A flipped ciphertext byte -> honest Tampered, zero files unpacked.
void tst_VaultE2E::tamperedCiphertextRefuses()
{
    ProfileVault *vault = ProfileVault::instance();
    seedProfileTree();
    QCOMPARE(vault->seal(kId, QByteArray("pw")), ProfileVault::NoError);

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
    QVERIFY(QDir(profileDir())
                .entryList(QDir::AllEntries | QDir::NoDotAndDotDot
                           | QDir::Hidden | QDir::System)
                .isEmpty());
    QCOMPARE(vault->state(kId), ProfileVault::Locked);
}

// Died unlocked -> next boot's scan fires dirtyDetected -> accepting
// the re-seal offer (re-collected password: a fresh process has no
// custody) seals the live plaintext; declining stays unlocked and
// disarms the marker honestly.
void tst_VaultE2E::dirtyCrashResealOffer()
{
    ProfileVault *vault = ProfileVault::instance();
    seedProfileTree();
    const QVariantMap before = snapshot(profileDir());
    QCOMPARE(vault->seal(kId, QByteArray("pw")), ProfileVault::NoError);
    QCOMPARE(vault->unseal(kId, QByteArray("pw")),
             ProfileVault::NoError);
    QCOMPARE(vault->state(kId), ProfileVault::DirtyUnlocked);

    // "Crash": the process dies — custody dies with it.
    vault->resetForTests();

    // Next boot: the scan surfaces exactly this profile.
    QSignalSpy spy(vault, &ProfileVault::dirtyDetected);
    vault->checkForDirtyProfiles();
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().first().toString(), kId);
    QCOMPARE(vault->dirtyProfiles(), QStringList() << kId);

    // Accept "Re-seal now": the offer's handler re-collects the
    // password and calls seal() — no custody survived the crash.
    QCOMPARE(vault->lockProfile(kId), ProfileVault::NoPassword);
    QCOMPARE(vault->seal(kId, QByteArray("pw")),
             ProfileVault::NoError);
    QCOMPARE(vault->state(kId), ProfileVault::Locked);
    QCOMPARE(vault->unseal(kId, QByteArray("pw")),
             ProfileVault::NoError);
    QCOMPARE(snapshot(profileDir()), before);

    // The decline path: acknowledged plaintext, marker disarmed.
    vault->resetForTests();
    QCOMPARE(vault->state(kId), ProfileVault::DirtyUnlocked);
    vault->disarmDirty(kId);
    QCOMPARE(vault->state(kId), ProfileVault::Unlocked);
    QVERIFY(vault->dirtyProfiles().isEmpty());
}

// Delete Profile on a locked profile wipes vault.arv without the
// password (owner-host honesty); on a dirty profile it wipes the
// artifacts and leaves the plaintext to the caller's ordinary
// profile-dir removal; open windows still refuse.
void tst_VaultE2E::deleteLockedProfileWipesVault()
{
    ProfileVault *vault = ProfileVault::instance();

    // No vault at all: no-op success, not an error.
    QCOMPARE(vault->wipeVault(kId), ProfileVault::NoError);

    // Locked: every artifact goes, no password needed.
    seedProfileTree();
    QCOMPARE(vault->seal(kId, QByteArray("pw")), ProfileVault::NoError);
    QCOMPARE(vault->state(kId), ProfileVault::Locked);
    QCOMPARE(vault->wipeVault(kId), ProfileVault::NoError);
    QCOMPARE(vault->state(kId), ProfileVault::NoVault);
    QVERIFY(!QFile::exists(vaultDir()));
    QVERIFY(!QFile::exists(vaultFile(QLatin1String("vault.arv"))));

    // Dirty-unlocked: artifacts + held exclusions wiped, the live
    // plaintext tree untouched (the manager deletes it as for any
    // unvaulted profile).
    seedProfileTree();
    QCOMPARE(vault->seal(kId, QByteArray("pw")), ProfileVault::NoError);
    QCOMPARE(vault->unseal(kId, QByteArray("pw")),
             ProfileVault::NoError);
    QCOMPARE(vault->state(kId), ProfileVault::DirtyUnlocked);
    QCOMPARE(vault->wipeVault(kId), ProfileVault::NoError);
    QVERIFY(!QFile::exists(vaultDir()));
    QVERIFY(QFile::exists(
        profileDir() + QLatin1String("/history.db")));
    QVERIFY(QFile::exists(profileDir()
        + QLatin1String("/cache/blob.bin"))); // restored from hold

    // Open windows refuse even the wipe.
    vault->setWindowGateForTests(true);
    QCOMPARE(vault->wipeVault(kId), ProfileVault::HasOpenWindows);
    vault->clearWindowGateForTests();
}

// Boot on a locked profile unpacks nothing: the dirty scan only
// fires for crash-survivor plaintext, the blob+marker are untouched,
// and no plaintext materializes.
void tst_VaultE2E::bootLeavesLockedProfileSealed()
{
    ProfileVault *vault = ProfileVault::instance();
    seedProfileTree();
    QCOMPARE(vault->seal(kId, QByteArray("pw")), ProfileVault::NoError);

    QSignalSpy spy(vault, &ProfileVault::dirtyDetected);
    vault->checkForDirtyProfiles();
    QCOMPARE(spy.count(), 0);
    QVERIFY(vault->dirtyProfiles().isEmpty());
    QCOMPARE(vault->state(kId), ProfileVault::Locked);
    QVERIFY(QFile::exists(vaultFile(QLatin1String("vault.arv"))));
    QVERIFY(QFile::exists(vaultFile(QLatin1String("vault.lock"))));
    QVERIFY(QDir(profileDir())
                .entryList(QDir::AllEntries | QDir::NoDotAndDotDot
                           | QDir::Hidden | QDir::System)
                .isEmpty());
}

QTEST_MAIN(tst_VaultE2E)
#include "tst_vaulte2e.moc"
