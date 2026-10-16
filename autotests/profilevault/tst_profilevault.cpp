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

#include <qdir.h>
#include <qfile.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qtemporarydir.h>

#include <filesystem>

#ifdef ARORA_RUSTCORE
#include <rustcore.h>
#endif

/*
    PVAULT01: FFI-level coverage for the rustcore profile vault —
    password-sealed, chunked, streamable "ARV1" archives of a profile
    directory.  These legs drive the C ABI directly: seal/verify/
    unseal/inspect/rewrap plus the honest error split (WrongPassword
    vs Tampered vs UnsupportedAlgo vs Corrupt), the progress
    callback, and the no-partial-output guarantee.
*/

class tst_ProfileVault : public QObject
{
    Q_OBJECT

private slots:
#ifdef ARORA_RUSTCORE
    void sealVerifyUnsealRoundTrip();
    void threefishRoundTrip();
    void wrongPasswordIsDistinct();
    void tamperLeavesNoOutput();
    void inspectReportsMetadata();
    void rewrapChangesPassword();
    void unknownAlgoUnsupported();
    void progressCallbackFires();
    void badArguments();
#else
    void noRustBuildInert();
#endif
};

#ifdef ARORA_RUSTCORE

static QByteArray utf8(const QString &s)
{
    return QFile::encodeName(s);
}

static RcStatus seal(const QString &dir, const QByteArray &pass,
                     uint8_t algo, const QString &out,
                     RvProgressCallback cb = nullptr,
                     void *ud = nullptr)
{
    const QByteArray d = utf8(dir);
    const QByteArray o = utf8(out);
    return rv_seal(d.constData(),
                   reinterpret_cast<const uint8_t *>(pass.constData()),
                   size_t(pass.size()), algo, o.constData(), cb, ud);
}

static RcStatus unseal(const QString &blob, const QByteArray &pass,
                       const QString &outDir,
                       RvProgressCallback cb = nullptr,
                       void *ud = nullptr)
{
    const QByteArray b = utf8(blob);
    const QByteArray o = utf8(outDir);
    return rv_unseal(b.constData(),
                     reinterpret_cast<const uint8_t *>(pass.constData()),
                     size_t(pass.size()), o.constData(), cb, ud);
}

static RcStatus verify(const QString &blob, const QByteArray &pass)
{
    const QByteArray b = utf8(blob);
    return rv_verify(b.constData(),
                     reinterpret_cast<const uint8_t *>(pass.constData()),
                     size_t(pass.size()));
}

// Recursive (name, payload) listing so the restored tree is compared
// against the fixture byte-for-byte, symlinks included.
static QMap<QString, QByteArray> treeListing(const QString &root)
{
    QMap<QString, QByteArray> out;
    QVector<QDir> stack = { QDir(root) };
    while (!stack.isEmpty()) {
        const QDir dir = stack.takeLast();
        const QFileInfoList entries = dir.entryInfoList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
        for (const QFileInfo &info : entries) {
            const QString key = QDir(root).relativeFilePath(
                info.absoluteFilePath());
            if (info.isSymLink()) {
                // Raw link payload — Qt's readLink/symLinkTarget
                // resolve it against the tree root and so differ
                // between src and restored copies.
                const std::string tgt =
                    std::filesystem::read_symlink(
                        std::filesystem::path(
                            info.absoluteFilePath().toStdString()))
                        .generic_string();
                out.insert(key + QLatin1String("@l"),
                           QByteArray(tgt.data(), qsizetype(tgt.size())));
            } else if (info.isDir()) {
                out.insert(key + QLatin1String("@d"), QByteArray());
                stack.append(QDir(info.absoluteFilePath()));
            } else {
                QFile f(info.absoluteFilePath());
                // An unreadable file records as an error marker —
                // the final QCOMPARE will still fail honestly.
                out.insert(key, f.open(QIODevice::ReadOnly)
                                    ? f.readAll()
                                    : QByteArray("<unreadable>"));
            }
        }
    }
    return out;
}

static void makeFixture(const QString &root)
{
    QDir().mkpath(root + QLatin1String("/sub/deep"));
    QFile a(root + QLatin1String("/a.txt"));
    QVERIFY(a.open(QIODevice::WriteOnly));
    a.write("alpha alpha alpha");
    a.close();
    QFile b(root + QLatin1String("/sub/b.bin"));
    QVERIFY(b.open(QIODevice::WriteOnly));
    b.write(QByteArray(5000, '\x07'));
    b.close();
    QFile c(root + QLatin1String("/sub/deep/c.txt"));
    QVERIFY(c.open(QIODevice::WriteOnly));
    c.write("cee");
    c.close();
    QDir().mkpath(root + QLatin1String("/emptydir"));
#ifndef Q_OS_WIN
    QVERIFY(QFile::link(QLatin1String("sub/b.bin"),
                        root + QLatin1String("/link")));
#endif
}

void tst_ProfileVault::sealVerifyUnsealRoundTrip()
{
    QTemporaryDir base;
    QVERIFY(base.isValid());
    const QString src = base.path() + QLatin1String("/profile");
    makeFixture(src);
    const QString vault = base.path() + QLatin1String("/v.arv");
    const QString out = base.path() + QLatin1String("/out");

    QCOMPARE(seal(src, "hunter2", RC_VAULT_ALGO_AES256_GCM, vault), RC_OK);
    QVERIFY(QFile::exists(vault));
    QCOMPARE(verify(vault, "hunter2"), RC_OK);
    QCOMPARE(unseal(vault, "hunter2", out), RC_OK);
    QCOMPARE(treeListing(src), treeListing(out));
}

void tst_ProfileVault::threefishRoundTrip()
{
    QTemporaryDir base;
    QVERIFY(base.isValid());
    const QString src = base.path() + QLatin1String("/profile");
    makeFixture(src);
    const QString vault = base.path() + QLatin1String("/v.arv");
    const QString out = base.path() + QLatin1String("/out");

    QCOMPARE(seal(src, "hunter2", RC_VAULT_ALGO_THREEFISH_XEX, vault),
             RC_OK);
    QCOMPARE(verify(vault, "hunter2"), RC_OK);
    QCOMPARE(unseal(vault, "hunter2", out), RC_OK);
    QCOMPARE(treeListing(src), treeListing(out));
}

void tst_ProfileVault::wrongPasswordIsDistinct()
{
    QTemporaryDir base;
    QVERIFY(base.isValid());
    const QString src = base.path() + QLatin1String("/profile");
    makeFixture(src);
    const QString vault = base.path() + QLatin1String("/v.arv");
    const QString out = base.path() + QLatin1String("/out");

    QCOMPARE(seal(src, "right", RC_VAULT_ALGO_AES256_GCM, vault), RC_OK);
    QCOMPARE(verify(vault, "wrong"), RC_WRONG_PASSWORD);
    QCOMPARE(unseal(vault, "wrong", out), RC_WRONG_PASSWORD);
    QVERIFY(!QFile::exists(out));
    // The error channel carries a human-readable reason.
    char *msg = rc_last_error_message();
    QVERIFY(msg && msg[0] != '\0');
    rc_string_free(msg);
}

void tst_ProfileVault::tamperLeavesNoOutput()
{
    QTemporaryDir base;
    QVERIFY(base.isValid());
    const QString src = base.path() + QLatin1String("/profile");
    makeFixture(src);
    const QString vault = base.path() + QLatin1String("/v.arv");
    const QString out = base.path() + QLatin1String("/out");

    QCOMPARE(seal(src, "hunter2", RC_VAULT_ALGO_AES256_GCM, vault),
             RC_OK);
    // Flip a byte deep inside the payload (past the 217-byte prefix).
    QFile f(vault);
    QVERIFY(f.open(QIODevice::ReadWrite));
    QVERIFY(f.seek(260));
    char byte = 0;
    QCOMPARE(f.read(&byte, 1), qint64(1));
    byte ^= 0x40;
    QVERIFY(f.seek(260));
    QCOMPARE(f.write(&byte, 1), qint64(1));
    f.close();

    QCOMPARE(verify(vault, "hunter2"), RC_TAMPERED);
    QCOMPARE(unseal(vault, "hunter2", out), RC_TAMPERED);
    QVERIFY(!QFile::exists(out));
    // No stray staging dirs either.
    const QStringList strays = QDir(base.path()).entryList(
        QStringList() << QLatin1String("*arv-unpack*"), QDir::Dirs);
    QVERIFY(strays.isEmpty());
}

void tst_ProfileVault::inspectReportsMetadata()
{
    QTemporaryDir base;
    QVERIFY(base.isValid());
    const QString src = base.path() + QLatin1String("/profile");
    makeFixture(src);
    const QString vault = base.path() + QLatin1String("/v.arv");
    QCOMPARE(seal(src, "hunter2", RC_VAULT_ALGO_AES256_GCM, vault),
             RC_OK);

    char *json = rv_inspect(utf8(vault).constData());
    QVERIFY(json);
    const QJsonObject o =
        QJsonDocument::fromJson(QByteArray(json)).object();
    rc_string_free(json);
    QCOMPARE(o.value(QLatin1String("format")).toString(),
             QLatin1String("ARV1"));
    QCOMPARE(o.value(QLatin1String("algo")).toInt(),
             int(RC_VAULT_ALGO_AES256_GCM));
    QCOMPARE(o.value(QLatin1String("kdf")).toString(),
             QLatin1String("argon2id"));
    QCOMPARE(o.value(QLatin1String("memory_kib")).toInt(), 65536);
    QCOMPARE(o.value(QLatin1String("chunk_count")).toInt() >= 1, true);
    // Header fields are unverified by definition — inspect never
    // claims authenticity.
    QCOMPARE(o.value(QLatin1String("authenticated")).toBool(), false);
}

void tst_ProfileVault::rewrapChangesPassword()
{
    QTemporaryDir base;
    QVERIFY(base.isValid());
    const QString src = base.path() + QLatin1String("/profile");
    makeFixture(src);
    const QString vault = base.path() + QLatin1String("/v.arv");
    const QString out = base.path() + QLatin1String("/out");
    QCOMPARE(seal(src, "old", RC_VAULT_ALGO_AES256_GCM, vault), RC_OK);
    const qint64 sizeBefore = QFileInfo(vault).size();

    const QByteArray oldp = "old";
    const QByteArray newp = "new";
    const QByteArray b = utf8(vault);
    QCOMPARE(rv_rewrap(b.constData(),
                       reinterpret_cast<const uint8_t *>(oldp.constData()),
                       size_t(oldp.size()),
                       reinterpret_cast<const uint8_t *>(newp.constData()),
                       size_t(newp.size())),
             RC_OK);
    // Cheap password change: file length unchanged (payload untouched).
    QCOMPARE(QFileInfo(vault).size(), sizeBefore);
    QCOMPARE(verify(vault, "old"), RC_WRONG_PASSWORD);
    QCOMPARE(verify(vault, "new"), RC_OK);
    QCOMPARE(unseal(vault, "new", out), RC_OK);
    QCOMPARE(treeListing(src), treeListing(out));

    // A wrong old password cannot rewrap.
    const QByteArray nope = "nope";
    QCOMPARE(rv_rewrap(b.constData(),
                       reinterpret_cast<const uint8_t *>(nope.constData()),
                       size_t(nope.size()),
                       reinterpret_cast<const uint8_t *>(newp.constData()),
                       size_t(newp.size())),
             RC_WRONG_PASSWORD);
}

void tst_ProfileVault::unknownAlgoUnsupported()
{
    QTemporaryDir base;
    QVERIFY(base.isValid());
    const QString src = base.path() + QLatin1String("/profile");
    makeFixture(src);
    const QString vault = base.path() + QLatin1String("/v.arv");
    QCOMPARE(seal(src, "hunter2", RC_VAULT_ALGO_AES256_GCM, vault),
             RC_OK);
    // Rewrite the algo byte (offset 4) to an unimplemented id.
    QFile f(vault);
    QVERIFY(f.open(QIODevice::ReadWrite));
    QVERIFY(f.seek(4));
    const char bad = char(0x77);
    QCOMPARE(f.write(&bad, 1), qint64(1));
    f.close();
    QCOMPARE(verify(vault, "hunter2"), RC_UNSUPPORTED_ALGO);
}

struct ProgressSpy {
    quint64 calls = 0;
    quint64 lastDone = 0;
    quint64 total = 0;
    bool monotonic = true;
};

static void progressSpy(uint64_t done, uint64_t total, void *ud)
{
    ProgressSpy *s = static_cast<ProgressSpy *>(ud);
    s->calls++;
    if (done < s->lastDone)
        s->monotonic = false;
    s->lastDone = done;
    s->total = total;
}

void tst_ProfileVault::progressCallbackFires()
{
    QTemporaryDir base;
    QVERIFY(base.isValid());
    const QString src = base.path() + QLatin1String("/profile");
    makeFixture(src);
    const QString vault = base.path() + QLatin1String("/v.arv");
    const QString out = base.path() + QLatin1String("/out");

    ProgressSpy spy;
    QCOMPARE(seal(src, "hunter2", RC_VAULT_ALGO_AES256_GCM, vault,
                  progressSpy, &spy),
             RC_OK);
    QVERIFY(spy.calls >= 2);
    QVERIFY(spy.monotonic);

    ProgressSpy uspy;
    QCOMPARE(unseal(vault, "hunter2", out, progressSpy, &uspy), RC_OK);
    QVERIFY(uspy.calls >= 2);
    QVERIFY(uspy.monotonic);
    QCOMPARE(uspy.lastDone, uspy.total);
}

void tst_ProfileVault::badArguments()
{
    QCOMPARE(rv_seal(nullptr,
                     reinterpret_cast<const uint8_t *>("x"), 1,
                     RC_VAULT_ALGO_AES256_GCM, "out.arv", nullptr, nullptr),
             RC_INVALID_ARGUMENT);
    QCOMPARE(rv_unseal("vault.arv",
                       reinterpret_cast<const uint8_t *>("x"), 1,
                       nullptr, nullptr, nullptr),
             RC_INVALID_ARGUMENT);
    QCOMPARE(rv_verify(nullptr,
                       reinterpret_cast<const uint8_t *>("x"), 1),
             RC_INVALID_ARGUMENT);
    QCOMPARE(rv_inspect(nullptr), (char *)nullptr);
    QCOMPARE(rv_rewrap("v.arv", nullptr, 4,
                       reinterpret_cast<const uint8_t *>("x"), 1),
             RC_INVALID_ARGUMENT);
    // Zero-length password is refused distinctly.
    QTemporaryDir base;
    QVERIFY(base.isValid());
    const QString src = base.path() + QLatin1String("/profile");
    makeFixture(src);
    QCOMPARE(seal(src, QByteArray(), RC_VAULT_ALGO_AES256_GCM,
                  base.path() + QLatin1String("/v.arv")),
             RC_EMPTY_PASSPHRASE);
}

#else // !ARORA_RUSTCORE

void tst_ProfileVault::noRustBuildInert()
{
    // Without the crate the vault surface does not exist; the suite
    // is a no-op so no-rust CI legs stay green.
    QVERIFY(true);
}

#endif

QTEST_MAIN(tst_ProfileVault)
#include "tst_profilevault.moc"
