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

#ifndef STUBVAULTENGINE_H
#define STUBVAULTENGINE_H

#include "profilevault.h"

#include <qcryptographichash.h>
#include <qdir.h>
#include <qdiriterator.h>
#include <qfile.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qrandom.h>
#include <qsavefile.h>

// PVAULT03 test seam — a REVERSIBLE, UNENCRYPTED vault packer driving
// the same engine contract the rv_* FFI will fill on merge.  It is
// only ever instantiated by autotests through ProfileVault's
// explicit-engine constructor; the production singleton uses
// defaultEngine().  Blob format ("ARVSTUB1\n" + JSON):
//   {salt, passHash=sha256(salt+pass), algo, mac=sha256(filesJson),
//    files:[{path,data base64}]}
// verify() checks structure -> Corrupt, payload MAC -> Tampered,
// password hash -> WrongPassword — the same honest splits the real
// engine reports.
class StubVaultEngine : public ProfileVaultEngine
{
public:
    bool available() const override { return true; }

    Status seal(const QString &dirPath, const QByteArray &password,
                int algo, const QString &outBlobPath,
                const ProgressFn &progress,
                const CancelFn &cancel) override
    {
        QJsonArray files;
        quint64 total = 0;
        QDirIterator it(dirPath,
                        QDir::Files | QDir::NoDotAndDotDot
                            | QDir::Hidden,
                        QDirIterator::Subdirectories);
        QStringList relPaths;
        while (it.hasNext()) {
            it.next();
            relPaths.append(it.fileInfo().filePath());
            total += quint64(qMax<qint64>(1, it.fileInfo().size()));
        }
        relPaths.sort();
        quint64 done = 0;
        const QDir root(dirPath);
        for (const QString &abs : relPaths) {
            if (cancel())
                return Canceled;
            QFile in(abs);
            if (!in.open(QIODevice::ReadOnly))
                return IoError;
            const QByteArray data = in.readAll();
            QJsonObject entry;
            entry.insert(QLatin1String("path"),
                         root.relativeFilePath(abs));
            entry.insert(QLatin1String("data"),
                         QString::fromLatin1(data.toBase64()));
            files.append(entry);
            done += quint64(qMax<qint64>(1, data.size()));
            if (progress)
                progress(done, total);
        }

        QByteArray salt(16, 0);
        QRandomGenerator::global()->fillRange(
            reinterpret_cast<quint32 *>(salt.data()), 4);
        salt = salt.toHex();
        QJsonObject blob;
        blob.insert(QLatin1String("format"), QLatin1String("ARVSTUB1"));
        blob.insert(QLatin1String("algo"), algo);
        blob.insert(QLatin1String("algo_name"),
                    algo == ProfileVault::AlgoThreefishXex
                        ? QLatin1String("Threefish-1024 XEX")
                        : QLatin1String("AES-256-GCM"));
        blob.insert(QLatin1String("salt"),
                    QString::fromLatin1(salt));
        blob.insert(QLatin1String("passHash"),
                    QString::fromLatin1(passHash(salt, password)));
        const QByteArray filesJson =
            QJsonDocument(files).toJson(QJsonDocument::Compact);
        blob.insert(QLatin1String("mac"),
                    QString::fromLatin1(macOf(filesJson)));
        blob.insert(QLatin1String("files"), files);

        // Atomic temp+rename, matching the FFI's guarantee.
        QSaveFile out(outBlobPath);
        if (!out.open(QIODevice::WriteOnly))
            return IoError;
        out.write(QJsonDocument(blob).toJson(QJsonDocument::Compact));
        return out.commit() ? EngineOk : IoError;
    }

    Status unseal(const QString &blobPath, const QByteArray &password,
                  const QString &outDirPath,
                  const ProgressFn &progress,
                  const CancelFn &cancel) override
    {
        QJsonObject blob;
        const Status checked = check(blobPath, password, &blob);
        if (checked != EngineOk)
            return checked;

        // No-partial: unpack into a staging sibling, rename only when
        // every file landed.
        const QString staging = outDirPath + QLatin1String(".staging");
        QDir(staging).removeRecursively();
        const QJsonArray files =
            blob.value(QLatin1String("files")).toArray();
        const quint64 total = quint64(files.size());
        quint64 done = 0;
        for (const QJsonValue &value : files) {
            if (cancel()) {
                QDir(staging).removeRecursively();
                return Canceled;
            }
            const QJsonObject entry = value.toObject();
            const QString rel =
                entry.value(QLatin1String("path")).toString();
            if (rel.contains(QLatin1String(".."))) {
                QDir(staging).removeRecursively();
                return Corrupt;
            }
            const QString target =
                staging + QLatin1Char('/') + rel;
            QFileInfo info(target);
            if (!QDir().mkpath(info.absolutePath())) {
                QDir(staging).removeRecursively();
                return IoError;
            }
            QFile out(target);
            if (!out.open(QIODevice::WriteOnly)) {
                QDir(staging).removeRecursively();
                return IoError;
            }
            out.write(QByteArray::fromBase64(
                entry.value(QLatin1String("data")).toString()
                    .toLatin1()));
            ++done;
            if (progress)
                progress(done, total);
        }
        QDir().mkpath(QFileInfo(outDirPath).absolutePath());
        if (QFile::exists(outDirPath))
            QDir(outDirPath).removeRecursively();
        if (!QDir().rename(staging, outDirPath)) {
            QDir(staging).removeRecursively();
            return IoError;
        }
        return EngineOk;
    }

    Status verify(const QString &blobPath,
                  const QByteArray &password) override
    {
        return check(blobPath, password, nullptr);
    }

    Status rewrap(const QString &blobPath,
                  const QByteArray &oldPassword,
                  const QByteArray &newPassword) override
    {
        QJsonObject blob;
        const Status checked = check(blobPath, oldPassword, &blob);
        if (checked != EngineOk)
            return checked;
        QByteArray salt(16, 0);
        QRandomGenerator::global()->fillRange(
            reinterpret_cast<quint32 *>(salt.data()), 4);
        salt = salt.toHex();
        blob.insert(QLatin1String("salt"), QString::fromLatin1(salt));
        blob.insert(QLatin1String("passHash"),
                    QString::fromLatin1(passHash(salt, newPassword)));
        QSaveFile out(blobPath);
        if (!out.open(QIODevice::WriteOnly))
            return IoError;
        out.write(QJsonDocument(blob).toJson(QJsonDocument::Compact));
        return out.commit() ? EngineOk : IoError;
    }

    QByteArray inspectJson(const QString &blobPath) override
    {
        QFile file(blobPath);
        if (!file.open(QIODevice::ReadOnly))
            return QByteArray();
        const QJsonObject blob =
            QJsonDocument::fromJson(file.readAll()).object();
        if (blob.value(QLatin1String("format")).toString()
            != QLatin1String("ARVSTUB1"))
            return QByteArray();
        QJsonObject out = blob;
        out.remove(QLatin1String("files"));
        out.remove(QLatin1String("passHash"));
        out.remove(QLatin1String("salt"));
        out.insert(QLatin1String("authenticated"), false);
        return QJsonDocument(out).toJson(QJsonDocument::Compact);
    }

private:
    static QByteArray passHash(const QByteArray &salt,
                               const QByteArray &password)
    {
        return QCryptographicHash::hash(
            salt + password, QCryptographicHash::Sha256).toHex();
    }

    static QByteArray macOf(const QByteArray &payload)
    {
        return QCryptographicHash::hash(
            payload, QCryptographicHash::Sha512).toHex();
    }

    // Structure -> Corrupt, payload MAC -> Tampered, password ->
    // WrongPassword — same ordering as the FFI's unwrap-then-MAC.
    Status check(const QString &blobPath, const QByteArray &password,
                 QJsonObject *blobOut)
    {
        QFile file(blobPath);
        if (!file.open(QIODevice::ReadOnly))
            return IoError;
        const QJsonDocument doc =
            QJsonDocument::fromJson(file.readAll());
        const QJsonObject blob = doc.object();
        if (blob.value(QLatin1String("format")).toString()
            != QLatin1String("ARVSTUB1")
            || !blob.contains(QLatin1String("files")))
            return Corrupt;
        const QByteArray filesJson = QJsonDocument(
            blob.value(QLatin1String("files")).toArray())
                .toJson(QJsonDocument::Compact);
        if (macOf(filesJson)
            != blob.value(QLatin1String("mac")).toString().toLatin1())
            return Tampered;
        if (passHash(
                blob.value(QLatin1String("salt")).toString().toLatin1(),
                password)
            != blob.value(QLatin1String("passHash"))
                   .toString().toLatin1())
            return WrongPassword;
        if (blobOut)
            *blobOut = blob;
        return EngineOk;
    }
};

#endif // STUBVAULTENGINE_H
