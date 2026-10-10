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

#include <autofillmanager.h>
#include <browserpaths.h>
#include <securestore.h>

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
    void tamperedStoreRejected();
    void passphraseCustody();
    void noPlaintextOnDisk();

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
#ifdef ARORA_RUSTCORE
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
    {
        AutoFillManager manager;
        manager.setForms(forms);
        flushSave(&manager);
        QVERIFY(QFile::exists(storePath()));
    }
    // Drop the in-RAM caches so the second manager reads the file,
    // not the store's decoded mirror.
    SecureStore::lock();
    {
        AutoFillManager manager;
        QVERIFY(sameForms(manager.forms(), forms));
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
#ifdef ARORA_RUSTCORE
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
#ifdef ARORA_RUSTCORE
    QVERIFY(QFile::exists(storePath()));
    QVERIFY(!QFile::exists(legacyPath()));
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
#ifdef ARORA_RUSTCORE
    QVERIFY(!QFile::exists(legacyPath()));
#endif
}

QTEST_MAIN(tst_AutoFill)
#include "tst_autofill.moc"
