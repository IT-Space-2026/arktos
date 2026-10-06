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
#include <qlibrary.h>
#include <qrandom.h>
#include <qsavefile.h>

#include <cstring>

#include <qdebug.h>

namespace {

// "ARSEC1" | 12-byte nonce | 16-byte GCM tag | ciphertext
const int kNonceSize = 12;
const int kTagSize = 16;
const int kHeaderSize = 6 + kNonceSize + kTagSize;

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

QByteArray loadOrCreateKey()
{
    const QString path =
        BrowserPaths::dataFilePath(QLatin1String("securestore.key"));
    QFile file(path);
    if (file.exists()) {
        if (file.open(QIODevice::ReadOnly)) {
            const QByteArray key = file.readAll();
            file.close();
            if (key.size() == 32) {
                // Tighten in case a previous run left wider perms.
                file.setPermissions(QFile::ReadUser | QFile::WriteUser);
                return key;
            }
            qWarning() << "SecureStore: ignoring corrupt key file"
                       << path;
        }
    }

    QByteArray key(32, Qt::Uninitialized);
    for (int i = 0; i < 4; ++i) {
        const quint64 v = QRandomGenerator::system()->generate64();
        std::memcpy(key.data() + i * 8, &v, 8);
    }
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

QByteArray encryptionKey()
{
    static const QByteArray key = loadOrCreateKey();
    return key;
}

} // namespace

bool SecureStore::isAvailable()
{
    return evp() && encryptionKey().size() == 32;
}

bool SecureStore::isSealed(const QByteArray &blob)
{
    return blob.startsWith(QByteArrayLiteral("ARSEC1"));
}

QByteArray SecureStore::seal(const QByteArray &plain)
{
    const EvpApi *e = evp();
    const QByteArray key = encryptionKey();
    if (!e || key.size() != 32)
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

QByteArray SecureStore::open(const QByteArray &blob, bool *ok)
{
    bool result = false;
    if (!ok)
        ok = &result;
    *ok = false;

    const EvpApi *e = evp();
    const QByteArray key = encryptionKey();
    if (!e || key.size() != 32
        || blob.size() < kHeaderSize || !isSealed(blob))
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
