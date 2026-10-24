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

#include <QtTest/QtTest>
#include "qtest_arktos.h"

#include <autofillmanager.h>
#include <browserpaths.h>
#include <securestore.h>

#ifdef ARKTOS_RUSTCORE
#include <autofilldialog.h>
#include <rustcore.h>
#include "rustcorebridge.h"
#endif

#include <qdatastream.h>
#include <qfile.h>
#include <qset.h>
#include <qsettings.h>
#include <qtemporarydir.h>

/*
    RCORE05: the saved-form store migrated into rustcore custody.

    Under CONFIG+=rustcore the records live in autofill-store.dat — a
    sealed ARSEC1 blob sharing the credential store's AEAD + Argon2id
    key (one unlock opens both).  These tests cover the round trip
    through the Rust store, the one-shot legacy autofill.dat import
    (sealed and plaintext variants), the passphrase lifecycle, the
    "no plaintext PII on disk" rule, and tamper rejection.

    Without the flag the same binary exercises the unchanged Qt path:
    sealed autofill.dat via the C++ SecureStore, with the Qt4-era
    plaintext reader still intact.
*/

class tst_AutoFill : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanupTestCase();

    void roundTrip();
    void sealedOnDisk();
    void legacySealedImport();
    void legacyPlaintextImport();
    void plaintextRetiredCopyErased();
    void tamperedStoreRejected();
    void passphraseCustody();
    void lockedStoreKeepsSealedFile();
    void noPlaintextOnDisk();
    void ffiStoreReachesDialog();

private:
    QTemporaryDir m_settingsDir;
};

static QString legacyPath()
{
    return BrowserPaths::dataFilePath(QLatin1String("autofill.dat"));
}

static QString retiredPath()
{
    return legacyPath() + QLatin1String(".migrated");
}

static QString storePath()
{
#ifdef ARKTOS_RUSTCORE
    return BrowserPaths::dataFilePath(QLatin1String("autofill-store.dat"));
#else
    return legacyPath();
#endif
}

static void removeStoreFiles()
{
    QFile::remove(storePath());
    QFile::remove(legacyPath());
    QFile::remove(retiredPath());
}

static AutoFillManager::Form sampleForm()
{
    AutoFillManager::Form form;
    form.url = QUrl(QStringLiteral("https://shop.example.com/login"));
    form.name = QLatin1String("loginform");
    form.hasAPassword = true;
    form.elements
        << AutoFillManager::Element(QLatin1String("user"),
                                    QLatin1String("alice"))
        << AutoFillManager::Element(QLatin1String("pass"),
                                    QLatin1String("s3cret"));
    return form;
}

static bool sameForms(const QList<AutoFillManager::Form> &a,
                      const QList<AutoFillManager::Form> &b)
{
    if (a.count() != b.count())
        return false;
    for (int i = 0; i < a.count(); ++i) {
        if (a.at(i).url != b.at(i).url || a.at(i).name != b.at(i).name
            || a.at(i).hasAPassword != b.at(i).hasAPassword)
            return false;
        if (QSet<AutoFillManager::Element>(a.at(i).elements.begin(),
                                           a.at(i).elements.end())
            != QSet<AutoFillManager::Element>(b.at(i).elements.begin(),
                                              b.at(i).elements.end()))
            return false;
    }
    return true;
}

// Serializes forms into the legacy QDataStream payload — the inner
// format both the sealed and the plaintext autofill.dat carry.
static QByteArray legacyPayload(const QList<AutoFillManager::Form> &forms)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream << forms;
    return payload;
}

// save() is a private slot fed by the AutoSaver; the meta-object
// still reaches it directly.
static void flushSave(AutoFillManager *manager)
{
    QVERIFY(QMetaObject::invokeMethod(manager, "save",
                                      Qt::DirectConnection));
}

void tst_AutoFill::initTestCase()
{
    QCoreApplication::setApplicationName(QStringLiteral("autofilltest"));
    QStandardPaths::setTestModeEnabled(true);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                     m_settingsDir.path());
    SecureStore::resetForTests();
    SecureStore::setInteractiveUnlockEnabled(false);
    removeStoreFiles();
}

void tst_AutoFill::init()
{
    SecureStore::resetForTests();
    SecureStore::setInteractiveUnlockEnabled(false);
    removeStoreFiles();
}

void tst_AutoFill::cleanupTestCase()
{
    SecureStore::resetForTests();
    removeStoreFiles();
}

// setForms -> autosave -> a fresh manager reads the records back
// through whichever store the build uses.
void tst_AutoFill::roundTrip()
{
    const QList<AutoFillManager::Form> forms{sampleForm()};
    AutoFillManager::Address address;
    address.country = QLatin1String("US");
    address.name = QLatin1String("Ada");
    address.street = QLatin1String("1 Main St");
    address.city = QLatin1String("Springfield");
    address.zip = QLatin1String("62701");
    address.lastUsed = 42;
    {
        AutoFillManager manager;
        manager.setForms(forms);
        manager.setAddresses(QList<AutoFillManager::Address>()
                             << address);
        flushSave(&manager);
        QVERIFY(QFile::exists(storePath()));
    }
    // Drop the in-RAM caches so the second manager reads the file,
    // not the store's decoded mirror.
    SecureStore::lock();
    {
        AutoFillManager manager;
        QVERIFY(sameForms(manager.forms(), forms));
        // ADDR01/02: the address list rides the same store — a store
        // that only kept forms used to drop it across the reload.
        QCOMPARE(manager.addresses().count(), 1);
        QCOMPARE(manager.addresses().first().name,
                 QLatin1String("Ada"));
        QCOMPARE(manager.addresses().first().zip,
                 QLatin1String("62701"));
        QCOMPARE(manager.addresses().first().lastUsed, 42);
    }
}

// The file on disk is a sealed ARSEC1 blob — the record payload is
// never serialized outside custody.
void tst_AutoFill::sealedOnDisk()
{
    {
        AutoFillManager manager;
        manager.setForms(QList<AutoFillManager::Form>() << sampleForm());
        flushSave(&manager);
    }
    QFile file(storePath());
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray raw = file.readAll();
    QVERIFY(SecureStore::isSealed(raw));
    QVERIFY(!raw.contains("alice"));
    QVERIFY(!raw.contains("s3cret"));
}

// A sealed legacy autofill.dat is imported once and retired.
void tst_AutoFill::legacySealedImport()
{
    const QList<AutoFillManager::Form> forms{sampleForm()};
    const QByteArray sealed = SecureStore::seal(legacyPayload(forms));
    QVERIFY(SecureStore::isSealed(sealed));
    {
        QFile file(legacyPath());
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(sealed);
    }

    {
        AutoFillManager manager;
        QVERIFY(sameForms(manager.forms(), forms));
    }
#ifdef ARKTOS_RUSTCORE
    // The new store holds the records; the consumed file is retired.
    QVERIFY(QFile::exists(storePath()));
    QVERIFY(!QFile::exists(legacyPath()));
    QVERIFY(QFile::exists(retiredPath()));
    SecureStore::lock();
    {
        AutoFillManager manager;
        QVERIFY(sameForms(manager.forms(), forms));
    }
#else
    // The Qt path keeps reading autofill.dat until the next save.
    QVERIFY(QFile::exists(legacyPath()));
#endif
}

// A Qt4-era plaintext file imports the same way.
void tst_AutoFill::legacyPlaintextImport()
{
    const QList<AutoFillManager::Form> forms{sampleForm()};
    {
        QFile file(legacyPath());
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(legacyPayload(forms));
    }

    {
        AutoFillManager manager;
        QVERIFY(sameForms(manager.forms(), forms));
    }
#ifdef ARKTOS_RUSTCORE
    QVERIFY(QFile::exists(storePath()));
    QVERIFY(!QFile::exists(legacyPath()));
    // SEC28: plaintext is erased, not renamed — no readable copy may
    // survive the import under any name.
    QVERIFY(!QFile::exists(retiredPath()));
#endif
}

// SEC28: an earlier import retired a plaintext legacy file by rename —
// the ".migrated" copy keeps credentials at rest in the clear, so the
// next load must erase it.  A sealed backup copy is ciphertext and
// stays.
void tst_AutoFill::plaintextRetiredCopyErased()
{
#ifdef ARKTOS_RUSTCORE
    // A populated Rust store means no import is attempted; the stale
    // plaintext backup must still be scrubbed by the load.
    const QList<AutoFillManager::Form> forms{sampleForm()};
    {
        AutoFillManager manager;
        manager.setForms(forms);
        flushSave(&manager);
    }
    QVERIFY(QFile::exists(storePath()));
    {
        QFile file(retiredPath());
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(legacyPayload(forms));
    }
    {
        AutoFillManager manager;
        QVERIFY(sameForms(manager.forms(), forms));
        QVERIFY(!QFile::exists(retiredPath()));
    }
    {
        QFile file(retiredPath());
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(SecureStore::seal(legacyPayload(forms)));
    }
    {
        AutoFillManager manager;
        QVERIFY(QFile::exists(retiredPath()));
    }
#else
    QSKIP("the .migrated retirement sweep is a rustcore-custody path");
#endif
}

// A flipped ciphertext byte fails authentication — the manager keeps
// an empty list rather than half-parsing garbage.
void tst_AutoFill::tamperedStoreRejected()
{
    {
        AutoFillManager manager;
        manager.setForms(QList<AutoFillManager::Form>() << sampleForm());
        flushSave(&manager);
    }
    const QString path = storePath();
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadWrite));
        QByteArray raw = file.readAll();
        raw[raw.size() - 1] = raw.at(raw.size() - 1) ^ 0x01;
        file.seek(0);
        file.write(raw);
    }
    // Force the store to re-read the file rather than its cache.
    SecureStore::lock();
    {
        AutoFillManager manager;
        QVERIFY(manager.forms().isEmpty());
    }
}

// The autofill store shares the credential custody lifecycle: locked
// means unreadable, wrong passphrase stays out, the right one opens
// both stores at once.
void tst_AutoFill::passphraseCustody()
{
    QVERIFY(SecureStore::enablePassphraseProtection(
        QLatin1String("passphrase1")));
    const QList<AutoFillManager::Form> forms{sampleForm()};
    {
        AutoFillManager manager;
        manager.setForms(forms);
        flushSave(&manager);
        QVERIFY(QFile::exists(storePath()));
    }

    SecureStore::lock();
    {
        AutoFillManager manager;
        QVERIFY(manager.forms().isEmpty());
    }
    QVERIFY(!SecureStore::unlock(QLatin1String("wrong")));
    {
        AutoFillManager manager;
        QVERIFY(manager.forms().isEmpty());
    }
    QVERIFY(SecureStore::unlock(QLatin1String("passphrase1")));
    {
        AutoFillManager manager;
        QVERIFY(sameForms(manager.forms(), forms));
    }
    QVERIFY(SecureStore::disablePassphraseProtection());
}

// SEC28 (no-rust path): a failed seal on a passphrase-protected
// store means the unlock was declined — the plaintext fallback that
// exists for a crypto-less system must NOT downgrade the sealed
// autofill.dat while the store is merely locked.
void tst_AutoFill::lockedStoreKeepsSealedFile()
{
#ifdef ARKTOS_RUSTCORE
    QSKIP("the plaintext fallback only exists in the Qt build");
#else
    QVERIFY(SecureStore::enablePassphraseProtection(
        QLatin1String("passphrase1")));
    AutoFillManager::Form form = sampleForm();
    form.hasAPassword = false;
    form.elements.clear();
    form.elements << AutoFillManager::Element(
        QLatin1String("color"), QLatin1String("blue"));
    {
        AutoFillManager manager;
        manager.setForms(QList<AutoFillManager::Form>() << form);
        flushSave(&manager);
    }
    {
        QFile file(storePath());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(SecureStore::isSealed(file.readAll()));
    }

    // Locked + non-interactive: the save must leave the sealed file
    // untouched rather than overwrite it in plaintext.
    SecureStore::lock();
    {
        AutoFillManager manager;
        QVERIFY(manager.forms().isEmpty());
        manager.setForms(QList<AutoFillManager::Form>() << form);
        flushSave(&manager);
    }
    {
        QFile file(storePath());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(SecureStore::isSealed(file.readAll()));
    }
    QVERIFY(SecureStore::unlock(QLatin1String("passphrase1")));
    QVERIFY(SecureStore::disablePassphraseProtection());
#endif
}

// PII fields (a card number here) must never land on disk unsealed:
// no plaintext legacy file is written, and the raw store bytes do
// not contain the value.
void tst_AutoFill::noPlaintextOnDisk()
{
    AutoFillManager::Form form = sampleForm();
    form.elements << AutoFillManager::Element(
        QLatin1String("cardnumber"), QLatin1String("4111111111111111"));
    {
        AutoFillManager manager;
        manager.setForms(QList<AutoFillManager::Form>() << form);
        flushSave(&manager);
    }
    QFile file(storePath());
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray raw = file.readAll();
    QVERIFY(!raw.contains("4111111111111111"));
    QVERIFY(!raw.contains("alice"));
#ifdef ARKTOS_RUSTCORE
    QVERIFY(!QFile::exists(legacyPath()));
#endif
}

// RC17: a record written straight through the C ABI surfaces in the
// dialog's model — the FFI and the UI read the same store.  The
// SecureStore custody bar holds on disk: sealed payload, owner-only
// file AND owner-only dir.
void tst_AutoFill::ffiStoreReachesDialog()
{
#ifdef ARKTOS_RUSTCORE
    // Drop a singleton an earlier leg may have left so the dialog's
    // manager loads the file the FFI write produces (its pending
    // changes, if any, flush before our write lands).
    AutoFillManager::releaseFor(BrowserPaths::activeProfileId());
    rustCoreEnsureDataDirFor(BrowserPaths::activeProfileId());

    const QByteArray forms = QByteArrayLiteral(
        "[{\"url\":\"https://ffi.example/login\",\"name\":\"f\","
        "\"has_password\":true,"
        "\"elements\":[[\"user\",\"ffi-user\"],[\"pass\",\"x\"]]}]");
    QCOMPARE(rc_autofill_set_forms(
                 reinterpret_cast<const uint8_t *>(forms.constData()),
                 size_t(forms.size())),
             RC_OK);
    const QByteArray addrs = QByteArrayLiteral(
        "[{\"name\":\"Grace\",\"country\":\"US\","
        "\"street\":\"9 Rust Way\",\"city\":\"Portland\","
        "\"zip\":\"97201\",\"phone\":\"+1 555 7777\","
        "\"email\":\"g@example.com\",\"last_used\":11}]");
    QCOMPARE(rc_autofill_set_addresses(
                 reinterpret_cast<const uint8_t *>(addrs.constData()),
                 size_t(addrs.size())),
             RC_OK);

    const QFileInfo storeInfo(storePath());
    QVERIFY(storeInfo.exists());
    const QFile::Permissions groupOther =
        QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup
        | QFile::ReadOther | QFile::WriteOther | QFile::ExeOther;
    QVERIFY(!(storeInfo.permissions() & groupOther));
    QVERIFY(!(QFileInfo(storeInfo.absolutePath()).permissions()
             & groupOther));

    AutoFillDialog dialog;
    QAbstractItemModel *model = dialog.tableView->model();
    QCOMPARE(model->rowCount(), 1);
    QCOMPARE(model->data(model->index(0, 0)).toString(),
             QLatin1String("ffi.example"));
    QCOMPARE(model->data(model->index(0, 1)).toString(),
             QLatin1String("ffi-user"));

    // The address half of the payload reads back through the manager.
    AutoFillManager manager;
    QCOMPARE(manager.addresses().count(), 1);
    QCOMPARE(manager.addresses().first().name, QLatin1String("Grace"));
    QCOMPARE(manager.addresses().first().zip, QLatin1String("97201"));
    QCOMPARE(manager.addresses().first().lastUsed, 11);
#else
    QSKIP("the autofill store FFI only exists in the rustcore build");
#endif
}

QTEST_MAIN(tst_AutoFill)
#include "tst_autofill.moc"
