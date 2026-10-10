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

#include "securestore.h"

#include "browserpaths.h"

#include <qfile.h>
#include <qsavefile.h>
#include <qsettings.h>

#ifdef QT_WIDGETS_LIB
#include <qapplication.h>
#include <qinputdialog.h>
#include <qlineedit.h>
#endif

#include <cstring>

#include <qdebug.h>

// RCORE01: under CONFIG+=rustcore the custody state machine and the
// AES-256-GCM/Argon2id crypto live in the rustcore crate behind the
// rc_* C ABI — the on-disk formats are identical, so either build
// reads the other's stores.  This file keeps the Qt-facing policy
// (interactive unlock prompt, consumer re-sealing via QSettings) and
// delegates the rest over FFI.  Without the flag the original
// OpenSSL-EVP + bundled-argon2id implementation below is used —
// the tree needs no Rust toolchain.
#ifdef ARORA_RUSTCORE
#include "rustcore.h"
#include "rustcorebridge.h"

#include <qcoreapplication.h>
#include <qfileinfo.h>
#else
#include "argon2id.h"

#include <qlibrary.h>
#include <qrandom.h>
#endif

namespace {

// "ARSEC1" | 12-byte nonce | 16-byte GCM tag | ciphertext
const int kNonceSize = 12;
const int kTagSize = 16;
const int kHeaderSize = 6 + kNonceSize + kTagSize;
const int kKeySize = 32;

void secureZero(void *data, qsizetype size)
{
    volatile unsigned char *p = static_cast<volatile unsigned char *>(data);
    while (size--)
        *p++ = 0;
}

void secureZero(QByteArray &bytes)
{
    secureZero(bytes.data(), bytes.size());
}

QString storePath(const char *name)
{
    return BrowserPaths::dataFilePath(QLatin1String(name));
}

// ---- shared interactive-unlock state -------------------------------

bool s_interactiveUnlock = true;
bool s_unlockPromptActive = false;

// Implemented per-backend below; resealConsumers() is backend-neutral.
QByteArray sealWithKey(const QByteArray &plain, const QByteArray &key);
QByteArray openWithKey(const QByteArray &blob, const QByteArray &key,
                       bool *ok);

// Re-seals every consumer store from oldKey to newKey.  Surfaces:
//   - autofill.dat (sealed blob or legacy plaintext)
//   - QSettings proxy/password ("arsec1:" blob or plaintext)
// A blob that does not open under oldKey is already unreadable — it
// is left in place with a warning rather than blocking the switch.
bool resealConsumers(const QByteArray &oldKey, const QByteArray &newKey,
                     QString *error)
{
    const QString autofillPath = storePath("autofill.dat");
    {
        QFile file(autofillPath);
        if (file.exists() && file.open(QIODevice::ReadOnly)) {
            const QByteArray raw = file.readAll();
            file.close();
            QByteArray resealed;
            if (SecureStore::isSealed(raw)) {
                bool ok = false;
                const QByteArray plain = openWithKey(raw, oldKey, &ok);
                if (ok)
                    resealed = sealWithKey(plain, newKey);
                else
                    qWarning() << "SecureStore: autofill.dat does not"
                                  " open under the previous key;"
                                  " leaving it in place";
            } else if (!raw.isEmpty()) {
                resealed = sealWithKey(raw, newKey);
            }
            if (!resealed.isEmpty()) {
                QSaveFile out(autofillPath);
                if (!out.open(QIODevice::WriteOnly)
                    || out.write(resealed) != resealed.size()
                    || !out.commit()) {
                    if (error)
                        *error = QStringLiteral(
                            "cannot rewrite the autofill store");
                    return false;
                }
                QFile::setPermissions(autofillPath,
                    QFile::ReadUser | QFile::WriteUser);
            }
        }
    }

    QSettings settings;
    settings.beginGroup(QLatin1String("proxy"));
    const QString stored =
        settings.value(QLatin1String("password")).toString();
    if (!stored.isEmpty()) {
        QString updated;
        static const QLatin1String prefix("arsec1:");
        if (stored.startsWith(prefix)) {
            bool ok = false;
            const QByteArray plain = openWithKey(
                QByteArray::fromBase64(
                    stored.mid(prefix.size()).toLatin1()),
                oldKey, &ok);
            if (ok) {
                updated = prefix + QString::fromLatin1(
                    sealWithKey(plain, newKey).toBase64());
            } else {
                qWarning() << "SecureStore: proxy password does not"
                              " open under the previous key;"
                              " leaving it in place";
            }
        } else {
            const QByteArray blob =
                sealWithKey(stored.toUtf8(), newKey);
            if (!blob.isEmpty())
                updated = prefix
                    + QString::fromLatin1(blob.toBase64());
        }
        if (!updated.isEmpty())
            settings.setValue(QLatin1String("password"), updated);
    }
    settings.endGroup();
    return true;
}

#ifdef ARORA_RUSTCORE

// ---- FFI-backed custody (rustcore crate) ---------------------------
//
// The Rust side owns the crypto and the key-file/KDF artifacts; this
// half owns the crash-safe transition ordering (write the new custody
// artifact -> re-seal every consumer -> retire the old one) and the
// interactive unlock prompt.

void ensureRustCore()
{
    // Cheap and idempotent — call per operation so test-mode data-dir
    // switches propagate.
    const QByteArray dir =
        QFileInfo(storePath("securestore.key")).absolutePath().toUtf8();
    rc_set_data_dir(dir.constData());
    // Wake the change-callback -> Qt-signal bridge once a real
    // application object exists.
    if (QCoreApplication::instance())
        RustCoreBridge::instance();
}

QByteArray bufferToByteArray(const RcBuffer &buf)
{
    const QByteArray out(reinterpret_cast<const char *>(buf.data),
                         qsizetype(buf.len));
    rc_buffer_free(buf);
    return out;
}

QByteArray sealWithKey(const QByteArray &plain, const QByteArray &key)
{
    ensureRustCore();
    if (key.size() != kKeySize)
        return QByteArray();
    RcBuffer out{};
    const RcStatus st = rc_seal_with_key(
        reinterpret_cast<const uint8_t *>(key.constData()),
        reinterpret_cast<const uint8_t *>(plain.constData()),
        size_t(plain.size()), &out);
    if (st != RC_OK)
        return QByteArray();
    return bufferToByteArray(out);
}

QByteArray openWithKey(const QByteArray &blob, const QByteArray &key,
                       bool *ok)
{
    ensureRustCore();
    bool result = false;
    if (!ok)
        ok = &result;
    *ok = false;
    if (key.size() != kKeySize)
        return QByteArray();
    RcBuffer out{};
    const RcStatus st = rc_open_with_key(
        reinterpret_cast<const uint8_t *>(key.constData()),
        reinterpret_cast<const uint8_t *>(blob.constData()),
        size_t(blob.size()), &out);
    if (st != RC_OK)
        return QByteArray();
    *ok = true;
    return bufferToByteArray(out);
}

// Copies of the custody keys for transition re-sealing.  The Rust
// side already caches the file key; these byte copies are wiped by
// the caller after use.
QByteArray fileKeyCopy()
{
    ensureRustCore();
    QByteArray key(kKeySize, Qt::Uninitialized);
    if (rc_key_file_copy(reinterpret_cast<uint8_t *>(key.data()))
        != RC_OK)
        return QByteArray();
    return key;
}

QByteArray derivedKeyCopy()
{
    QByteArray key(kKeySize, Qt::Uninitialized);
    if (rc_derived_key_copy(reinterpret_cast<uint8_t *>(key.data()))
        != RC_OK)
        return QByteArray();
    return key;
}

// The Rust-owned consumers (credentials.dat, autofill-store.dat)
// re-seal through the same hook the other consumers use.
// Warn-and-continue matches the unreadable-blob policy everywhere
// else.
void resealRustStores(const QByteArray &oldKey,
                      const QByteArray &newKey)
{
    if (oldKey.size() != kKeySize || newKey.size() != kKeySize)
        return;
    const RcStatus st = rc_cred_reseal(
        reinterpret_cast<const uint8_t *>(oldKey.constData()),
        reinterpret_cast<const uint8_t *>(newKey.constData()));
    if (st != RC_OK && st != RC_NOT_FOUND)
        qWarning() << "SecureStore: credentials.dat does not open"
                      " under the previous key; leaving it in place";
    const RcStatus stAuto = rc_autofill_reseal(
        reinterpret_cast<const uint8_t *>(oldKey.constData()),
        reinterpret_cast<const uint8_t *>(newKey.constData()));
    if (stAuto != RC_OK && stAuto != RC_NOT_FOUND)
        qWarning() << "SecureStore: autofill-store.dat does not open"
                      " under the previous key; leaving it in place";
}

void wipeKeys(QByteArray &a, QByteArray &b)
{
    secureZero(a);
    secureZero(b);
}

#else // !ARORA_RUSTCORE — the original OpenSSL-EVP implementation

// Argon2id parameters for the master-passphrase KDF — RFC 9106's
// memory-constrained recommendation (64 MiB / t=3 / p=4).  Stored in
// securestore.kdf so they can be raised later.
const quint32 kKdfMemoryKiB = 65536;
const quint32 kKdfPasses = 3;
const quint32 kKdfLanes = 4;
const int kKdfSaltSize = 16;
const char kKdfMagic[] = "ARKDF1";
const char kVerifierMessage[] = "ARORA-KDF-VERIFY";

// EVP_CTRL_GCM_GET_TAG / EVP_CTRL_GCM_SET_TAG values are stable ABI
// constants (identical since OpenSSL 1.0.1 through 3.x).
const int kEvpCtrlGcmGetTag = 0x10;
const int kEvpCtrlGcmSetTag = 0x11;

typedef void *(*EvpCtxNewFn)();
typedef void (*EvpCtxFreeFn)(void *);
typedef const void *(*EvpAes256GcmFn)();
typedef int (*EvpCryptInitFn)(void *, const void *, void *,
                              const unsigned char *,
                              const unsigned char *);
typedef int (*EvpCryptUpdateFn)(void *, unsigned char *, int *,
                                const unsigned char *, int);
typedef int (*EvpCryptFinalFn)(void *, unsigned char *, int *);
typedef int (*EvpCtxCtrlFn)(void *, int, int, void *);

struct EvpApi {
    EvpCtxNewFn ctxNew = nullptr;
    EvpCtxFreeFn ctxFree = nullptr;
    EvpAes256GcmFn aes256Gcm = nullptr;
    EvpCryptInitFn encryptInit = nullptr;
    EvpCryptUpdateFn encryptUpdate = nullptr;
    EvpCryptFinalFn encryptFinal = nullptr;
    EvpCryptInitFn decryptInit = nullptr;
    EvpCryptUpdateFn decryptUpdate = nullptr;
    EvpCryptFinalFn decryptFinal = nullptr;
    EvpCtxCtrlFn ctxCtrl = nullptr;
};

bool resolveEvp(EvpApi *api)
{
    // libcrypto ships a runtime SONAME on essentially every Linux
    // distro but no development package is required to dlopen it.
    // The QLibrary is intentionally leaked: the resolved pointers must
    // stay valid until process exit, and unloading libcrypto while
    // another consumer (QSsl) shares it would be worse.
    static const char *versions[] = { "3", "1.1", "1.0.2", "" };
    static QLibrary *library = new QLibrary;
    for (const char *version : versions) {
        library->setFileNameAndVersion(QLatin1String("crypto"),
                                       QLatin1String(version));
        if (library->load())
            break;
    }
    if (!library->isLoaded())
        return false;

    api->ctxNew = reinterpret_cast<EvpCtxNewFn>(
        library->resolve("EVP_CIPHER_CTX_new"));
    api->ctxFree = reinterpret_cast<EvpCtxFreeFn>(
        library->resolve("EVP_CIPHER_CTX_free"));
    api->aes256Gcm = reinterpret_cast<EvpAes256GcmFn>(
        library->resolve("EVP_aes_256_gcm"));
    api->encryptInit = reinterpret_cast<EvpCryptInitFn>(
        library->resolve("EVP_EncryptInit_ex"));
    api->encryptUpdate = reinterpret_cast<EvpCryptUpdateFn>(
        library->resolve("EVP_EncryptUpdate"));
    api->encryptFinal = reinterpret_cast<EvpCryptFinalFn>(
        library->resolve("EVP_EncryptFinal_ex"));
    api->decryptInit = reinterpret_cast<EvpCryptInitFn>(
        library->resolve("EVP_DecryptInit_ex"));
    api->decryptUpdate = reinterpret_cast<EvpCryptUpdateFn>(
        library->resolve("EVP_DecryptUpdate"));
    api->decryptFinal = reinterpret_cast<EvpCryptFinalFn>(
        library->resolve("EVP_DecryptFinal_ex"));
    api->ctxCtrl = reinterpret_cast<EvpCtxCtrlFn>(
        library->resolve("EVP_CIPHER_CTX_ctrl"));

    return api->ctxNew && api->ctxFree && api->aes256Gcm
        && api->encryptInit && api->encryptUpdate && api->encryptFinal
        && api->decryptInit && api->decryptUpdate && api->decryptFinal
        && api->ctxCtrl;
}

const EvpApi *evp()
{
    static EvpApi api;
    static const bool resolved = resolveEvp(&api);
    return resolved ? &api : nullptr;
}

// ---- key custody state ---------------------------------------------
// The passphrase-derived key lives only in RAM (s_derivedKey); the
// on-disk artifacts are either securestore.key (random file key) or
// securestore.kdf (public salt+params+verifier) — never both.

unsigned char s_derivedKey[kKeySize];
bool s_derivedKeyValid = false;
QByteArray s_fileKey;
bool s_fileKeyLoaded = false;

struct KdfParams {
    quint32 memoryKiB;
    quint32 passes;
    quint32 lanes;
    QByteArray salt;
    QByteArray verifier;
};

QByteArray randomBytes(int count)
{
    QByteArray bytes(count, Qt::Uninitialized);
    for (int i = 0; i + 8 <= count; i += 8) {
        const quint64 v = QRandomGenerator::system()->generate64();
        std::memcpy(bytes.data() + i, &v, 8);
    }
    return bytes;
}

QByteArray loadOrCreateFileKey()
{
    const QString path = storePath("securestore.key");
    QFile file(path);
    if (file.exists()) {
        if (file.open(QIODevice::ReadOnly)) {
            const QByteArray key = file.readAll();
            file.close();
            if (key.size() == kKeySize) {
                // Tighten in case a previous run left wider perms.
                file.setPermissions(QFile::ReadUser | QFile::WriteUser);
                return key;
            }
            qWarning() << "SecureStore: ignoring corrupt key file"
                       << path;
        }
    }

    QByteArray key = randomBytes(kKeySize);
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly)) {
        qWarning() << "SecureStore: cannot create key file" << path;
        return QByteArray();
    }
    out.write(key);
    if (!out.commit()) {
        qWarning() << "SecureStore: cannot commit key file" << path;
        return QByteArray();
    }
    QFile::setPermissions(path, QFile::ReadUser | QFile::WriteUser);
    return key;
}

// Cached file key.  Note this stays loaded across a transition to
// passphrase mode until the transition finishes — the old key is
// needed to re-seal consumer stores — then lock() drops it.
const QByteArray &fileKey()
{
    if (!s_fileKeyLoaded) {
        s_fileKey = loadOrCreateFileKey();
        s_fileKeyLoaded = true;
    }
    return s_fileKey;
}

bool parseKdfFile(KdfParams *params)
{
    QFile file(storePath("securestore.kdf"));
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray data = file.readAll();
    // magic 6 | m u32 | t u32 | p u32 | salt 16 | verifier blob
    if (data.size() < 6 + 12 + kKdfSaltSize + kHeaderSize
        || !data.startsWith(kKdfMagic))
        return false;
    auto readU32 = [&data](int at) -> quint32 {
        return quint32(quint8(data.at(at)))
            | (quint32(quint8(data.at(at + 1))) << 8)
            | (quint32(quint8(data.at(at + 2))) << 16)
            | (quint32(quint8(data.at(at + 3))) << 24);
    };
    params->memoryKiB = readU32(6);
    params->passes = readU32(10);
    params->lanes = readU32(14);
    params->salt = data.mid(18, kKdfSaltSize);
    params->verifier = data.mid(34);
    return params->salt.size() == kKdfSaltSize
        && SecureStore::isSealed(params->verifier);
}

bool writeKdfFile(const KdfParams &params)
{
    QByteArray data;
    data.reserve(34 + params.verifier.size());
    data.append(kKdfMagic, 6);
    auto putU32 = [&data](quint32 v) {
        data.append(char(v & 0xff));
        data.append(char((v >> 8) & 0xff));
        data.append(char((v >> 16) & 0xff));
        data.append(char((v >> 24) & 0xff));
    };
    putU32(params.memoryKiB);
    putU32(params.passes);
    putU32(params.lanes);
    data.append(params.salt);
    data.append(params.verifier);

    const QString path = storePath("securestore.kdf");
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(data) != data.size() || !file.commit()) {
        qWarning() << "SecureStore: cannot write" << path;
        return false;
    }
    QFile::setPermissions(path, QFile::ReadUser | QFile::WriteUser);
    return true;
}

QByteArray deriveKey(const QString &passphrase, const KdfParams &params)
{
    QByteArray utf8 = passphrase.toUtf8();
    QByteArray key(kKeySize, Qt::Uninitialized);
    const int rc = arora_argon2id(key.data(), size_t(key.size()),
        utf8.constData(), size_t(utf8.size()),
        params.salt.constData(), size_t(params.salt.size()),
        params.passes, params.memoryKiB, params.lanes);
    secureZero(utf8);
    if (rc != 0) {
        secureZero(key);
        return QByteArray();
    }
    return key;
}

// The key the current custody mode seals/opens with.  In passphrase
// mode a locked store tries one interactive unlock first and fails
// securely when the user declines.
QByteArray currentKey()
{
    if (SecureStore::passphraseProtectionEnabled()) {
        if (!s_derivedKeyValid)
            SecureStore::ensureUnlocked(nullptr);
        if (!s_derivedKeyValid)
            return QByteArray();
        return QByteArray(reinterpret_cast<char *>(s_derivedKey),
                          kKeySize);
    }
    return fileKey();
}

QByteArray sealWithKey(const QByteArray &plain, const QByteArray &key)
{
    const EvpApi *e = evp();
    if (!e || key.size() != kKeySize)
        return QByteArray();

    unsigned char nonce[kNonceSize];
    for (int i = 0; i < 3; ++i) {
        const quint32 v = QRandomGenerator::system()->generate();
        std::memcpy(nonce + i * 4, &v, 4);
    }

    void *ctx = e->ctxNew();
    if (!ctx)
        return QByteArray();

    QByteArray cipher(plain.size() + kTagSize, Qt::Uninitialized);
    unsigned char tag[kTagSize];
    int len = 0;
    int total = 0;
    bool ok = e->encryptInit(ctx, e->aes256Gcm(), nullptr,
                             reinterpret_cast<const unsigned char *>(
                                 key.constData()),
                             nonce) == 1;
    if (ok && !plain.isEmpty()) {
        ok = e->encryptUpdate(ctx,
                              reinterpret_cast<unsigned char *>(
                                  cipher.data()),
                              &len,
                              reinterpret_cast<const unsigned char *>(
                                  plain.constData()),
                              plain.size()) == 1;
        total = len;
    }
    if (ok) {
        ok = e->encryptFinal(ctx,
                             reinterpret_cast<unsigned char *>(
                                 cipher.data()) + total,
                             &len) == 1;
        total += len;
    }
    if (ok)
        ok = e->ctxCtrl(ctx, kEvpCtrlGcmGetTag, kTagSize, tag) == 1;
    e->ctxFree(ctx);
    if (!ok)
        return QByteArray();

    cipher.truncate(total);
    QByteArray blob;
    blob.reserve(kHeaderSize + total);
    blob.append("ARSEC1", 6);
    blob.append(reinterpret_cast<const char *>(nonce), kNonceSize);
    blob.append(reinterpret_cast<const char *>(tag), kTagSize);
    blob.append(cipher);
    return blob;
}

QByteArray openWithKey(const QByteArray &blob, const QByteArray &key,
                       bool *ok)
{
    bool result = false;
    if (!ok)
        ok = &result;
    *ok = false;

    const EvpApi *e = evp();
    if (!e || key.size() != kKeySize
        || blob.size() < kHeaderSize || !SecureStore::isSealed(blob))
        return QByteArray();

    const unsigned char *nonce =
        reinterpret_cast<const unsigned char *>(blob.constData() + 6);
    const unsigned char *tag =
        reinterpret_cast<const unsigned char *>(
            blob.constData() + 6 + kNonceSize);
    const QByteArray cipher = blob.mid(kHeaderSize);

    void *ctx = e->ctxNew();
    if (!ctx)
        return QByteArray();

    QByteArray plain(cipher.size() + kTagSize, Qt::Uninitialized);
    int len = 0;
    int total = 0;
    bool good = e->decryptInit(ctx, e->aes256Gcm(), nullptr,
                               reinterpret_cast<const unsigned char *>(
                                   key.constData()),
                               nonce) == 1;
    // GCM requires the tag to be set before the ciphertext is fed in.
    if (good)
        good = e->ctxCtrl(ctx, kEvpCtrlGcmSetTag, kTagSize,
                          const_cast<unsigned char *>(tag)) == 1;
    if (good && !cipher.isEmpty()) {
        good = e->decryptUpdate(ctx,
                                reinterpret_cast<unsigned char *>(
                                    plain.data()),
                                &len,
                                reinterpret_cast<const unsigned char *>(
                                    cipher.constData()),
                                cipher.size()) == 1;
        total = len;
    }
    if (good) {
        // DecryptFinal_ex returning <= 0 means the tag did not verify.
        good = e->decryptFinal(ctx,
                               reinterpret_cast<unsigned char *>(
                                   plain.data()) + total,
                               &len) == 1;
        total += len;
    }
    e->ctxFree(ctx);
    if (!good)
        return QByteArray();

    plain.truncate(total);
    *ok = true;
    return plain;
}

#endif // ARORA_RUSTCORE

} // namespace

#ifdef ARORA_RUSTCORE

bool SecureStore::isAvailable()
{
    ensureRustCore();
    return rc_is_available() != 0;
}

QByteArray SecureStore::seal(const QByteArray &plain)
{
    ensureRustCore();
    // Locked passphrase store: try one interactive unlock first and
    // fail securely when the user declines — same policy as before.
    if (rc_passphrase_enabled() && !rc_is_unlocked())
        ensureUnlocked(nullptr);
    RcBuffer out{};
    if (rc_seal(reinterpret_cast<const uint8_t *>(plain.constData()),
                size_t(plain.size()), &out) != RC_OK)
        return QByteArray();
    return bufferToByteArray(out);
}

QByteArray SecureStore::open(const QByteArray &blob, bool *ok)
{
    bool result = false;
    if (!ok)
        ok = &result;
    *ok = false;

    ensureRustCore();
    if (rc_passphrase_enabled() && !rc_is_unlocked())
        ensureUnlocked(nullptr);

    RcBuffer out{};
    // Rust performs the passphrase-mode file-key fallback internally.
    if (rc_open(reinterpret_cast<const uint8_t *>(blob.constData()),
                size_t(blob.size()), &out) != RC_OK)
        return QByteArray();
    *ok = true;
    return bufferToByteArray(out);
}

bool SecureStore::passphraseProtectionEnabled()
{
    ensureRustCore();
    return rc_passphrase_enabled() != 0;
}

bool SecureStore::isUnlocked()
{
    ensureRustCore();
    return rc_is_unlocked() != 0;
}

bool SecureStore::unlock(const QString &passphrase)
{
    ensureRustCore();
    QByteArray utf8 = passphrase.toUtf8();
    const RcStatus st = rc_unlock(
        reinterpret_cast<const uint8_t *>(utf8.constData()),
        size_t(utf8.size()));
    secureZero(utf8);
    return st == RC_OK;
}

void SecureStore::lock()
{
    rc_lock();
}

bool SecureStore::enablePassphraseProtection(const QString &passphrase,
                                             QString *error)
{
    ensureRustCore();
    if (passphraseProtectionEnabled()) {
        if (error)
            *error = QStringLiteral("passphrase protection is already"
                                    " enabled");
        return false;
    }
    if (passphrase.isEmpty()) {
        if (error)
            *error = QStringLiteral("empty passphrase");
        return false;
    }

    // Capture the current (file) key before custody switches.
    QByteArray oldKey = fileKeyCopy();
    if (oldKey.size() != kKeySize) {
        if (error)
            *error = QStringLiteral("cannot create the key file");
        return false;
    }

    QByteArray utf8 = passphrase.toUtf8();
    const RcStatus st = rc_kdf_create(
        reinterpret_cast<const uint8_t *>(utf8.constData()),
        size_t(utf8.size()));
    secureZero(utf8);
    if (st != RC_OK) {
        secureZero(oldKey);
        if (error)
            *error = st == RC_IO
                ? QStringLiteral("cannot write the KDF parameters")
                : QStringLiteral("key derivation failed");
        return false;
    }
    QByteArray newKey = derivedKeyCopy();

    // Consumers re-seal while securestore.key still exists — a crash
    // here still leaves open()'s file-key fallback able to read the
    // old blobs; the key file is retired last.
    const bool resealed = resealConsumers(oldKey, newKey, error);
    if (!resealed) {
        wipeKeys(oldKey, newKey);
        return false;
    }
    resealRustStores(oldKey, newKey);

    rc_key_file_delete();
    wipeKeys(oldKey, newKey);
    return true;
}

bool SecureStore::disablePassphraseProtection(QString *error)
{
    ensureRustCore();
    if (!passphraseProtectionEnabled()) {
        if (error)
            *error = QStringLiteral("passphrase protection is not"
                                    " enabled");
        return false;
    }
    if (!isUnlocked()) {
        if (error)
            *error = QStringLiteral("the store is locked — unlock it"
                                    " first");
        return false;
    }

    QByteArray oldKey = derivedKeyCopy();
    // Mirror ordering: write the new key file, re-seal consumers off
    // the derived key, then drop the KDF file and wipe the key from
    // RAM.
    if (rc_key_file_create() != RC_OK) {
        secureZero(oldKey);
        if (error)
            *error = QStringLiteral("cannot write the key file");
        return false;
    }
    QByteArray newKey = fileKeyCopy();

    const bool resealed = resealConsumers(oldKey, newKey, error);
    if (resealed)
        resealRustStores(oldKey, newKey);
    if (!resealed) {
        wipeKeys(oldKey, newKey);
        return false;
    }

    rc_kdf_file_delete();
    wipeKeys(oldKey, newKey);
    return true;
}

bool SecureStore::changePassphrase(const QString &newPassphrase,
                                   QString *error)
{
    ensureRustCore();
    if (!passphraseProtectionEnabled()) {
        if (error)
            *error = QStringLiteral("passphrase protection is not"
                                    " enabled");
        return false;
    }
    if (!isUnlocked()) {
        if (error)
            *error = QStringLiteral("the store is locked — unlock it"
                                    " first");
        return false;
    }
    if (newPassphrase.isEmpty()) {
        if (error)
            *error = QStringLiteral("empty passphrase");
        return false;
    }

    QByteArray oldKey = derivedKeyCopy();
    QByteArray utf8 = newPassphrase.toUtf8();
    const RcStatus st = rc_kdf_create(
        reinterpret_cast<const uint8_t *>(utf8.constData()),
        size_t(utf8.size()));
    secureZero(utf8);
    if (st != RC_OK) {
        secureZero(oldKey);
        if (error)
            *error = st == RC_IO
                ? QStringLiteral("cannot write the KDF parameters")
                : QStringLiteral("key derivation failed");
        return false;
    }
    QByteArray newKey = derivedKeyCopy();

    const bool resealed = resealConsumers(oldKey, newKey, error);
    if (resealed)
        resealRustStores(oldKey, newKey);
    wipeKeys(oldKey, newKey);
    return resealed;
}

#else // !ARORA_RUSTCORE

bool SecureStore::isAvailable()
{
    return evp()
        && (passphraseProtectionEnabled() || fileKey().size() == kKeySize);
}

QByteArray SecureStore::seal(const QByteArray &plain)
{
    QByteArray key = currentKey();
    QByteArray blob = sealWithKey(plain, key);
    secureZero(key);
    return blob;
}

QByteArray SecureStore::open(const QByteArray &blob, bool *ok)
{
    bool result = false;
    if (!ok)
        ok = &result;
    *ok = false;

    QByteArray key = currentKey();
    QByteArray plain = openWithKey(blob, key, &result);

    // Transition residue fallback: in passphrase mode a blob sealed
    // before the migration (or by an interrupted transition) still
    // opens under the on-disk key while it exists — the data stays
    // readable and is rewritten under the derived key on next save.
    // The exists() check keeps fileKey() from creating a fresh key
    // file in passphrase mode just to fail the retry.
    if (!result && passphraseProtectionEnabled()
        && QFile::exists(storePath("securestore.key"))) {
        const QByteArray legacy = fileKey();
        if (legacy.size() == kKeySize)
            plain = openWithKey(blob, legacy, &result);
    }
    secureZero(key);
    if (!result)
        return QByteArray();
    *ok = true;
    return plain;
}

bool SecureStore::passphraseProtectionEnabled()
{
    return QFile::exists(storePath("securestore.kdf"));
}

bool SecureStore::isUnlocked()
{
    return !passphraseProtectionEnabled() || s_derivedKeyValid;
}

bool SecureStore::unlock(const QString &passphrase)
{
    KdfParams params;
    if (!parseKdfFile(&params))
        return false;

    QByteArray candidate = deriveKey(passphrase, params);
    if (candidate.size() != kKeySize)
        return false;

    // The verifier is a sealed known-plaintext blob — a wrong key
    // fails the GCM tag like any tampered ciphertext, no oracle.
    bool ok = false;
    const QByteArray plain = openWithKey(params.verifier, candidate, &ok);
    if (!ok || plain != kVerifierMessage) {
        secureZero(candidate);
        return false;
    }
    std::memcpy(s_derivedKey, candidate.constData(), kKeySize);
    s_derivedKeyValid = true;
    secureZero(candidate);
    return true;
}

void SecureStore::lock()
{
    if (s_derivedKeyValid) {
        secureZero(s_derivedKey, kKeySize);
        s_derivedKeyValid = false;
    }
    if (s_fileKeyLoaded) {
        secureZero(s_fileKey);
        s_fileKey.clear();
        s_fileKeyLoaded = false;
    }
}

bool SecureStore::enablePassphraseProtection(const QString &passphrase,
                                             QString *error)
{
    if (!evp()) {
        if (error)
            *error = QStringLiteral("crypto backend unavailable");
        return false;
    }
    if (passphraseProtectionEnabled()) {
        if (error)
            *error = QStringLiteral("passphrase protection is already"
                                    " enabled");
        return false;
    }
    if (passphrase.isEmpty()) {
        if (error)
            *error = QStringLiteral("empty passphrase");
        return false;
    }

    // Capture the current (file) key before custody switches.
    QByteArray oldKey = fileKey();
    if (oldKey.size() != kKeySize) {
        if (error)
            *error = QStringLiteral("cannot create the key file");
        return false;
    }

    KdfParams params;
    params.memoryKiB = kKdfMemoryKiB;
    params.passes = kKdfPasses;
    params.lanes = kKdfLanes;
    params.salt = randomBytes(kKdfSaltSize);

    QByteArray newKey = deriveKey(passphrase, params);
    if (newKey.size() != kKeySize) {
        if (error)
            *error = QStringLiteral("key derivation failed");
        return false;
    }
    params.verifier = sealWithKey(
        QByteArray(kVerifierMessage), newKey);
    if (params.verifier.isEmpty()) {
        secureZero(newKey);
        if (error)
            *error = QStringLiteral("cannot seal the verifier");
        return false;
    }

    // Order matters against a crash: the params file is committed
    // atomically first so the verifier+salt exist, the derived key is
    // cached, the consumers are re-sealed under it, and the key file
    // is removed last — any interruption leaves open()'s file-key
    // fallback able to read the old blobs.
    if (!writeKdfFile(params)) {
        secureZero(newKey);
        if (error)
            *error = QStringLiteral("cannot write the KDF parameters");
        return false;
    }
    std::memcpy(s_derivedKey, newKey.constData(), kKeySize);
    s_derivedKeyValid = true;

    if (!resealConsumers(oldKey, newKey, error)) {
        secureZero(newKey);
        return false;
    }

    QFile::remove(storePath("securestore.key"));
    if (s_fileKeyLoaded) {
        secureZero(s_fileKey);
        s_fileKey.clear();
        s_fileKeyLoaded = false;
    }
    secureZero(oldKey);
    secureZero(newKey);
    return true;
}

bool SecureStore::disablePassphraseProtection(QString *error)
{
    if (!passphraseProtectionEnabled()) {
        if (error)
            *error = QStringLiteral("passphrase protection is not"
                                    " enabled");
        return false;
    }
    if (!s_derivedKeyValid) {
        if (error)
            *error = QStringLiteral("the store is locked — unlock it"
                                    " first");
        return false;
    }

    QByteArray oldKey(reinterpret_cast<char *>(s_derivedKey), kKeySize);
    const QByteArray newKey = randomBytes(kKeySize);

    // Mirror ordering: write the new key file, re-seal consumers off
    // the derived key, then drop the KDF file and wipe the key from
    // RAM.
    const QString keyPath = storePath("securestore.key");
    {
        QSaveFile out(keyPath);
        if (!out.open(QIODevice::WriteOnly)
            || out.write(newKey) != newKey.size() || !out.commit()) {
            if (error)
                *error = QStringLiteral("cannot write the key file");
            return false;
        }
        QFile::setPermissions(keyPath,
            QFile::ReadUser | QFile::WriteUser);
    }

    if (!resealConsumers(oldKey, newKey, error)) {
        secureZero(oldKey);
        return false;
    }

    QFile::remove(storePath("securestore.kdf"));
    s_fileKey = newKey;
    s_fileKeyLoaded = true;
    secureZero(s_derivedKey, kKeySize);
    s_derivedKeyValid = false;
    secureZero(oldKey);
    return true;
}

bool SecureStore::changePassphrase(const QString &newPassphrase,
                                   QString *error)
{
    if (!passphraseProtectionEnabled()) {
        if (error)
            *error = QStringLiteral("passphrase protection is not"
                                    " enabled");
        return false;
    }
    if (!s_derivedKeyValid) {
        if (error)
            *error = QStringLiteral("the store is locked — unlock it"
                                    " first");
        return false;
    }
    if (newPassphrase.isEmpty()) {
        if (error)
            *error = QStringLiteral("empty passphrase");
        return false;
    }

    QByteArray oldKey(reinterpret_cast<char *>(s_derivedKey), kKeySize);

    KdfParams params;
    params.memoryKiB = kKdfMemoryKiB;
    params.passes = kKdfPasses;
    params.lanes = kKdfLanes;
    params.salt = randomBytes(kKdfSaltSize);

    QByteArray newKey = deriveKey(newPassphrase, params);
    if (newKey.size() != kKeySize) {
        secureZero(oldKey);
        if (error)
            *error = QStringLiteral("key derivation failed");
        return false;
    }
    params.verifier = sealWithKey(
        QByteArray(kVerifierMessage), newKey);

    if (!writeKdfFile(params)) {
        secureZero(oldKey);
        secureZero(newKey);
        if (error)
            *error = QStringLiteral("cannot write the KDF parameters");
        return false;
    }
    std::memcpy(s_derivedKey, newKey.constData(), kKeySize);

    const bool resealed = resealConsumers(oldKey, newKey, error);
    secureZero(oldKey);
    secureZero(newKey);
    return resealed;
}

#endif // ARORA_RUSTCORE

// ---- backend-neutral API --------------------------------------------

bool SecureStore::isSealed(const QByteArray &blob)
{
    return blob.startsWith(QByteArrayLiteral("ARSEC1"));
}

QString SecureStore::sealString(const QString &plain)
{
    const QByteArray blob = seal(plain.toUtf8());
    if (blob.isEmpty())
        return plain;
    return QLatin1String("arsec1:")
        + QString::fromLatin1(blob.toBase64());
}

QString SecureStore::openString(const QString &stored, bool *ok)
{
    if (ok)
        *ok = true;
    static const QLatin1String prefix("arsec1:");
    if (!stored.startsWith(prefix))
        return stored;
    bool good = false;
    const QByteArray plain = open(
        QByteArray::fromBase64(stored.mid(prefix.size()).toLatin1()),
        &good);
    if (ok)
        *ok = good;
    return good ? QString::fromUtf8(plain) : QString();
}

bool SecureStore::ensureUnlocked(QWidget *parent)
{
    if (isUnlocked())
        return true;
    if (!s_interactiveUnlock)
        return false;
#ifdef QT_WIDGETS_LIB
    if (!qobject_cast<QApplication *>(QCoreApplication::instance())
        || s_unlockPromptActive)
        return false;
    s_unlockPromptActive = true;
    bool unlocked = false;
    for (int attempt = 0; attempt < 3 && !unlocked; ++attempt) {
        bool given = false;
        const QString text = QInputDialog::getText(parent,
            QCoreApplication::translate("SecureStore",
                                        "Unlock Credential Store"),
            attempt == 0
                ? QCoreApplication::translate("SecureStore",
                    "Enter the master passphrase to unlock the saved"
                    " password store:")
                : QCoreApplication::translate("SecureStore",
                    "Wrong passphrase — try again:"),
            QLineEdit::Password, QString(), &given);
        if (!given)
            break;
        unlocked = unlock(text);
    }
    s_unlockPromptActive = false;
    return unlocked;
#else
    Q_UNUSED(parent);
    return false;
#endif
}

void SecureStore::setInteractiveUnlockEnabled(bool enabled)
{
    s_interactiveUnlock = enabled;
}

bool SecureStore::isInteractiveUnlockEnabled()
{
    return s_interactiveUnlock;
}

#ifdef AUTOTESTS
void SecureStore::resetForTests()
{
    lock();
    s_interactiveUnlock = true;
    s_unlockPromptActive = false;
#ifdef ARORA_RUSTCORE
    ensureRustCore();
    rc_kdf_file_delete();
    rc_key_file_delete();
    QFile::remove(storePath(RC_CREDENTIALS_FILE));
    QFile::remove(storePath(RC_AUTOFILL_FILE));
#else
    QFile::remove(storePath("securestore.kdf"));
    QFile::remove(storePath("securestore.key"));
#endif
}
#endif
