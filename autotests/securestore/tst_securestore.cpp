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

#include <argon2id.h>
#include <browserpaths.h>
#include <securestore.h>

#include <qfile.h>
#include <qlineedit.h>
#include <qlibrary.h>
#include <qsettings.h>
#include <qtemporarydir.h>

/*
    SEC13: master-passphrase custody for the credential store.

    The sealed-blob format (ARSEC1 / AES-256-GCM) is unchanged from
    SEC03; these tests cover the optional Argon2id-derived key mode:
    enable/disable/change transitions re-seal consumer data, wrong
    passphrases fail through the GCM verifier, tampered ciphertext is
    rejected, and no key material is written to disk in passphrase
    mode.
*/

class tst_SecureStore : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanupTestCase();

    void argon2idVector();
    void argon2idMatchesSystemLib();

    void fileKeyRoundTrip();
    void passphraseRoundTrip();
    void noKeyFileInPassphraseMode();
    void noKeyMaterialInKdfFile();
    void wrongPassphraseFailsCleanly();
    void tamperedCiphertextRejected();
    void lockUnlockCycle();
    void autofillDatResealed();
    void proxyPasswordResealed();
    void changePassphraseRotatesKey();
    void disableRestoresFileKey();
    void interactiveUnlockPrompt();
    void nonInteractiveUnlockFails();

private:
    QTemporaryDir m_settingsDir;
};

static QString kdfPath()
{
    return BrowserPaths::dataFilePath(QLatin1String("securestore.kdf"));
}

static QString keyPath()
{
    return BrowserPaths::dataFilePath(QLatin1String("securestore.key"));
}

static QString autofillPath()
{
    return BrowserPaths::dataFilePath(QLatin1String("autofill.dat"));
}

void tst_SecureStore::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    // Keep proxy/password QSettings out of the user's real ini files.
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       m_settingsDir.path());
    SecureStore::resetForTests();
}

void tst_SecureStore::init()
{
    SecureStore::resetForTests();
    SecureStore::setInteractiveUnlockEnabled(false);
    QFile::remove(autofillPath());
}

void tst_SecureStore::cleanupTestCase()
{
    SecureStore::resetForTests();
}

// Verified against the system's libargon2 argon2id_hash_raw — the
// RFC 9106 vectors exercise secret/AD inputs this API does not have,
// so a libargon2-produced value is the pinned reference here.
void tst_SecureStore::argon2idVector()
{
    QByteArray password(32, '\x01');
    QByteArray salt(16, '\x02');
    QByteArray out(32, Qt::Uninitialized);
    QCOMPARE(arora_argon2id(out.data(), size_t(out.size()),
                            password.constData(), size_t(password.size()),
                            salt.constData(), size_t(salt.size()),
                            3, 32, 4), 0);
    QCOMPARE(out.toHex(), QByteArray(
        "03aab965c12001c9d7d0d2de33192c04"
        "94b684bb148196d73c1df1acaf6d0c2e"));
}

// Differential check: where libargon2.so.1 exists the bundled
// implementation must agree across parameter shapes.  Unix-only —
// the vendored code is itself the portability claim, so there is
// nothing to compare against on platforms without the system lib.
void tst_SecureStore::argon2idMatchesSystemLib()
{
#ifdef Q_OS_UNIX
    typedef int (*HashRawFn)(uint32_t, uint32_t, uint32_t,
                             const void *, size_t,
                             const void *, size_t,
                             void *, size_t);
    QLibrary lib(QLatin1String("argon2"), 1);
    if (!lib.load())
        QSKIP("libargon2.so.1 not installed — nothing to diff against");
    HashRawFn raw = reinterpret_cast<HashRawFn>(
        lib.resolve("argon2id_hash_raw"));
    QVERIFY(raw != nullptr);

    struct Case { uint32_t t, m, p; int plen, slen; };
    const Case cases[] = {
        {3, 32, 4, 32, 16},
        {1, 8, 1, 1, 8},
        {2, 46, 3, 5, 9},
        {4, 1024, 4, 16, 16},
    };
    for (const Case &c : cases) {
        QByteArray password(c.plen, 'p');
        QByteArray salt(c.slen, 's');
        QByteArray mine(32, Qt::Uninitialized);
        QByteArray theirs(32, Qt::Uninitialized);
        QCOMPARE(arora_argon2id(mine.data(), 32,
                                password.constData(), size_t(c.plen),
                                salt.constData(), size_t(c.slen),
                                c.t, c.m, c.p), 0);
        QCOMPARE(raw(c.t, c.m, c.p,
                     password.constData(), size_t(c.plen),
                     salt.constData(), size_t(c.slen),
                     theirs.data(), 32), 0);
        QCOMPARE(mine, theirs);
    }
#else
    QSKIP("system libargon2 comparison only runs on Unix");
#endif
}

void tst_SecureStore::fileKeyRoundTrip()
{
    QVERIFY(SecureStore::isAvailable());
    QVERIFY(!SecureStore::passphraseProtectionEnabled());

    const QByteArray blob = SecureStore::seal("hello");
    QVERIFY(SecureStore::isSealed(blob));
    bool ok = false;
    QCOMPARE(SecureStore::open(blob, &ok), QByteArray("hello"));
    QVERIFY(ok);
    QVERIFY(QFile::exists(keyPath()));
}

void tst_SecureStore::passphraseRoundTrip()
{
    QVERIFY(SecureStore::enablePassphraseProtection(
        QLatin1String("correct horse battery")));
    QVERIFY(SecureStore::passphraseProtectionEnabled());
    QVERIFY(SecureStore::isUnlocked());

    const QByteArray blob = SecureStore::seal("secret data");
    QVERIFY(SecureStore::isSealed(blob));
    bool ok = false;
    QCOMPARE(SecureStore::open(blob, &ok), QByteArray("secret data"));
    QVERIFY(ok);
}

void tst_SecureStore::noKeyFileInPassphraseMode()
{
    // Touch the file key once so it definitely exists, then enable —
    // passphrase mode must remove it and never recreate it.
    QVERIFY(SecureStore::isAvailable());
    QVERIFY(QFile::exists(keyPath()));

    QVERIFY(SecureStore::enablePassphraseProtection(
        QLatin1String("passphrase1")));
    QVERIFY(!QFile::exists(keyPath()));
    QVERIFY(QFile::exists(kdfPath()));

    // Locked opens fail securely rather than materializing a key.
    SecureStore::lock();
    bool ok = true;
    SecureStore::open(SecureStore::seal("x"), &ok); // seal fails too
    QVERIFY(!ok);
    QVERIFY(!QFile::exists(keyPath()));
}

void tst_SecureStore::noKeyMaterialInKdfFile()
{
    QVERIFY(SecureStore::enablePassphraseProtection(
        QLatin1String("passphrase1")));

    // Recover the salt from the kdf file header and re-derive the key
    // directly — then prove that key appears nowhere in the file.
    QFile file(kdfPath());
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray data = file.readAll();
    file.close();
    QVERIFY(data.startsWith("ARKDF1"));
    const QByteArray salt = data.mid(18, 16);
    QCOMPARE(salt.size(), 16);

    const QByteArray pass = QLatin1String("passphrase1").toUtf8();
    QByteArray derived(32, Qt::Uninitialized);
    QCOMPARE(arora_argon2id(derived.data(), 32,
                            pass.constData(), size_t(pass.size()),
                            salt.constData(), size_t(salt.size()),
                            3, 65536, 4), 0);
    QVERIFY(!data.contains(derived));
    QVERIFY(!QFile::exists(keyPath()));

    // And the passphrase itself is nowhere in the file either.
    QVERIFY(!data.contains(pass));
}

void tst_SecureStore::wrongPassphraseFailsCleanly()
{
    QVERIFY(SecureStore::enablePassphraseProtection(
        QLatin1String("right passphrase")));
    SecureStore::lock();
    QVERIFY(!SecureStore::isUnlocked());
    QVERIFY(!SecureStore::unlock(QLatin1String("wrong passphrase")));
    QVERIFY(!SecureStore::isUnlocked());
    QVERIFY(SecureStore::unlock(QLatin1String("right passphrase")));
    QVERIFY(SecureStore::isUnlocked());
}

void tst_SecureStore::tamperedCiphertextRejected()
{
    QVERIFY(SecureStore::enablePassphraseProtection(
        QLatin1String("passphrase1")));
    QByteArray blob = SecureStore::seal("payload");
    QVERIFY(SecureStore::isSealed(blob));
    bool ok = false;
    QCOMPARE(SecureStore::open(blob, &ok), QByteArray("payload"));
    QVERIFY(ok);

    blob[blob.size() - 1] = blob.at(blob.size() - 1) ^ 0x01;
    ok = true;
    QVERIFY(SecureStore::open(blob, &ok).isEmpty());
    QVERIFY(!ok);
}

void tst_SecureStore::lockUnlockCycle()
{
    QVERIFY(SecureStore::enablePassphraseProtection(
        QLatin1String("passphrase1")));
    const QByteArray blob = SecureStore::seal("data");

    SecureStore::lock();
    QVERIFY(!SecureStore::isUnlocked());

    // Locked: both directions fail without prompting (non-interactive).
    bool ok = true;
    QVERIFY(SecureStore::open(blob, &ok).isEmpty());
    QVERIFY(!ok);
    QVERIFY(SecureStore::seal("more").isEmpty());
    QVERIFY(!SecureStore::ensureUnlocked());

    QVERIFY(SecureStore::unlock(QLatin1String("passphrase1")));
    ok = false;
    QCOMPARE(SecureStore::open(blob, &ok), QByteArray("data"));
    QVERIFY(ok);
}

void tst_SecureStore::autofillDatResealed()
{
    // Data sealed under the file key survives the switch to the
    // derived key byte-for-byte on open().
    {
        QFile out(autofillPath());
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write(SecureStore::seal("autofill contents"));
    }
    QVERIFY(SecureStore::enablePassphraseProtection(
        QLatin1String("passphrase1")));

    QFile in(autofillPath());
    QVERIFY(in.open(QIODevice::ReadOnly));
    const QByteArray raw = in.readAll();
    in.close();
    QVERIFY(SecureStore::isSealed(raw));
    bool ok = false;
    QCOMPARE(SecureStore::open(raw, &ok),
             QByteArray("autofill contents"));
    QVERIFY(ok);
}

void tst_SecureStore::proxyPasswordResealed()
{
    QSettings settings;
    settings.setValue(QLatin1String("proxy/password"),
                      SecureStore::sealString(
                          QLatin1String("s3cret")));
    settings.sync();

    QVERIFY(SecureStore::enablePassphraseProtection(
        QLatin1String("passphrase1")));

    QSettings verify;
    const QString stored = verify.value(
        QLatin1String("proxy/password")).toString();
    QVERIFY(stored.startsWith(QLatin1String("arsec1:")));
    bool ok = false;
    QCOMPARE(SecureStore::openString(stored, &ok),
             QLatin1String("s3cret"));
    QVERIFY(ok);
    settings.remove(QLatin1String("proxy/password"));
}

void tst_SecureStore::changePassphraseRotatesKey()
{
    QVERIFY(SecureStore::enablePassphraseProtection(
        QLatin1String("old passphrase")));
    {
        QFile out(autofillPath());
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write(SecureStore::seal("data"));
    }

    QVERIFY(SecureStore::changePassphrase(
        QLatin1String("new passphrase")));

    // The persisted store is re-sealed under the rotated key — the
    // old passphrase no longer unlocks anything.
    SecureStore::lock();
    QVERIFY(!SecureStore::unlock(QLatin1String("old passphrase")));
    QVERIFY(SecureStore::unlock(QLatin1String("new passphrase")));

    QFile in(autofillPath());
    QVERIFY(in.open(QIODevice::ReadOnly));
    const QByteArray raw = in.readAll();
    in.close();
    bool ok = false;
    QCOMPARE(SecureStore::open(raw, &ok), QByteArray("data"));
    QVERIFY(ok);
}

void tst_SecureStore::disableRestoresFileKey()
{
    QVERIFY(SecureStore::enablePassphraseProtection(
        QLatin1String("passphrase1")));
    {
        QFile out(autofillPath());
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write(SecureStore::seal("data"));
    }

    QVERIFY(SecureStore::disablePassphraseProtection());
    QVERIFY(!SecureStore::passphraseProtectionEnabled());
    QVERIFY(QFile::exists(keyPath()));
    QVERIFY(!QFile::exists(kdfPath()));

    QFile in(autofillPath());
    QVERIFY(in.open(QIODevice::ReadOnly));
    const QByteArray raw = in.readAll();
    in.close();
    bool ok = false;
    QCOMPARE(SecureStore::open(raw, &ok), QByteArray("data"));
    QVERIFY(ok);
}

// Types into the QInputDialog password field and accepts — offscreen
// platform runs real modal loops, so a scheduled helper does the
// typing while getText() is exec()d.
static void answerModalPassphrase(const QString &text, int msec = 50)
{
    QTimer::singleShot(msec, qApp, [text]() {
        QWidget *widget = QApplication::activeModalWidget();
        if (!widget)
            return;
        if (QLineEdit *edit = widget->findChild<QLineEdit *>())
            edit->setText(text);
        if (QDialog *dialog = qobject_cast<QDialog *>(widget))
            dialog->accept();
    });
}

void tst_SecureStore::interactiveUnlockPrompt()
{
    QVERIFY(SecureStore::enablePassphraseProtection(
        QLatin1String("passphrase1")));
    SecureStore::lock();
    SecureStore::setInteractiveUnlockEnabled(true);

    // User cancels — the store stays locked and seal/open fail.
    rejectModal();
    QVERIFY(!SecureStore::ensureUnlocked());
    QVERIFY(!SecureStore::isUnlocked());

    // User enters the passphrase — open() prompts on first access.
    answerModalPassphrase(QLatin1String("passphrase1"));
    QVERIFY(SecureStore::ensureUnlocked());
    QVERIFY(SecureStore::isUnlocked());
}

void tst_SecureStore::nonInteractiveUnlockFails()
{
    QVERIFY(SecureStore::enablePassphraseProtection(
        QLatin1String("passphrase1")));
    SecureStore::lock();
    // init() already disabled interactive unlock — no dialog may
    // appear and the request must fail cleanly.
    QVERIFY(!SecureStore::ensureUnlocked());
    QVERIFY(!SecureStore::isUnlocked());
}

QTEST_MAIN(tst_SecureStore)
#include "tst_securestore.moc"
