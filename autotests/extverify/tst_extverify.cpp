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

// EXT06: extension-package verification — the rc_ext_verify_package /
// rc_ext_manifest_check FFI verdicts (valid/unsigned/signed/rejected,
// declared-hash gate, traversal member names, CRX3 structure), the
// ExtensionManager::inspectManifest pre-validated-manifest path, and
// the update manifest's hash_sha256 plumbing.  Fixtures are built in
// code — a minimal stored-only zip writer plus hand-encoded CRX3
// protobuf headers.  Under a no-rust build the tests pin the
// degradation contract instead of skipping outright.

#include <QtTest/QtTest>

#include "extensionmanager.h"
#include "qtest_arora.h"

#if defined(ARORA_RUSTCORE)
#include <rustcore.h>
#endif

// Minimal stored-only zip writer: local headers + central dir + EOCD.
static QByteArray buildZip(const QList<QPair<QByteArray, QByteArray>> &entries)
{
    QByteArray out;
    QList<quint32> offsets;
    const auto put16 = [&out](quint16 v) {
        out.append(char(v & 0xFF));
        out.append(char(v >> 8));
    };
    const auto put32 = [&out](quint32 v) {
        out.append(char(v & 0xFF));
        out.append(char((v >> 8) & 0xFF));
        out.append(char((v >> 16) & 0xFF));
        out.append(char(v >> 24));
    };
    for (const auto &entry : entries) {
        offsets.append(quint32(out.size()));
        put32(0x04034b50);
        put16(20); put16(0); put16(0); put16(0); put16(0);
        put32(0);
        put32(quint32(entry.second.size()));
        put32(quint32(entry.second.size()));
        put16(quint16(entry.first.size()));
        put16(0);
        out.append(entry.first);
        out.append(entry.second);
    }
    const quint32 cdOffset = quint32(out.size());
    for (int i = 0; i < entries.size(); ++i) {
        const auto &entry = entries.at(i);
        put32(0x02014b50);
        put16(20); put16(20); put16(0); put16(0); put16(0); put16(0);
        put32(0);
        put32(quint32(entry.second.size()));
        put32(quint32(entry.second.size()));
        put16(quint16(entry.first.size()));
        put16(0); put16(0); put16(0); put16(0);
        put32(0);
        put32(offsets.at(i));
        out.append(entry.first);
    }
    const quint32 cdSize = quint32(out.size()) - cdOffset;
    put32(0x06054b50);
    put16(0); put16(0);
    put16(quint16(entries.size()));
    put16(quint16(entries.size()));
    put32(cdSize);
    put32(cdOffset);
    put16(0);
    return out;
}

// Minimal protobuf length-delimited field encoder for the CRX3 header.
static QByteArray pbBytes(quint64 field, const QByteArray &body)
{
    QByteArray out;
    out.append(char(field << 3 | 2));
    quint64 len = quint64(body.size());
    do {
        char b = char(len & 0x7F);
        len >>= 7;
        if (len)
            b = char(b | 0x80);
        out.append(b);
    } while (len);
    out.append(body);
    return out;
}

static QByteArray buildCrx3(const QByteArray &headerBody,
                            const QByteArray &payload)
{
    QByteArray out = QByteArrayLiteral("Cr24");
    const quint32 version = 3;
    const quint32 hlen = quint32(headerBody.size());
    out.append(char(version & 0xFF)).append(char(0)).append(char(0)).append(char(0));
    out.append(char(hlen & 0xFF))
        .append(char((hlen >> 8) & 0xFF))
        .append(char((hlen >> 16) & 0xFF))
        .append(char(hlen >> 24));
    out.append(headerBody);
    out.append(payload);
    return out;
}

static const QByteArray kManifest = QByteArrayLiteral(
    "{\"manifest_version\":3,\"name\":\"T\",\"version\":\"1.0\","
    "\"permissions\":[\"storage\"]}");

static QByteArray packageZip()
{
    return buildZip({
        { QByteArrayLiteral("manifest.json"), kManifest },
        { QByteArrayLiteral("bg.js"), QByteArrayLiteral("//x") },
    });
}

#if defined(ARORA_RUSTCORE)
// Calls rc_ext_verify_package and returns the verdict as a JSON object.
static QJsonObject verifyPackage(const QByteArray &pkg,
                                 const QByteArray &expectedHash = QByteArray())
{
    RcBuffer out{};
    const RcStatus status = rc_ext_verify_package(
        reinterpret_cast<const uint8_t *>(pkg.constData()),
        size_t(pkg.size()),
        expectedHash.size() == 32
            ? reinterpret_cast<const uint8_t *>(expectedHash.constData())
            : nullptr,
        nullptr, 0, &out);
    QJsonObject verdict;
    if (status == RC_OK)
        verdict = QJsonDocument::fromJson(
            QByteArray(reinterpret_cast<const char *>(out.data),
                       qsizetype(out.len))).object();
    rc_buffer_free(out);
    return verdict;
}
#endif

class tst_ExtVerify : public QObject
{
    Q_OBJECT

public slots:
    void initTestCase();
    void cleanupTestCase();

private slots:
    void packageVerify();
    void manifestCheck();
    void inspectManifestPackages();
    void updateManifestHash();
};

void tst_ExtVerify::initTestCase()
{
}

void tst_ExtVerify::cleanupTestCase()
{
}

void tst_ExtVerify::packageVerify()
{
#if defined(ARORA_RUSTCORE)
    // A valid unsigned zip: classification, not failure — manifest
    // comes back pre-validated.
    {
        const QJsonObject v = verifyPackage(packageZip());
        QCOMPARE(v.value(QLatin1String("status")).toString(),
                 QLatin1String("valid"));
        QCOMPARE(v.value(QLatin1String("format")).toString(),
                 QLatin1String("zip"));
        QCOMPARE(v.value(QLatin1String("signing")).toString(),
                 QLatin1String("unsigned"));
        QCOMPARE(v.value(QLatin1String("entries")).toInt(), 2);
        QCOMPARE(v.value(QLatin1String("manifest"))
                     .toObject().value(QLatin1String("name")).toString(),
                 QLatin1String("T"));
    }
    // Declared-hash gate: match passes, a tampered package rejects.
    {
        const QByteArray zip = packageZip();
        const QByteArray good = QCryptographicHash::hash(
            zip, QCryptographicHash::Sha256);
        QJsonObject v = verifyPackage(zip, good);
        QCOMPARE(v.value(QLatin1String("expected_sha256")).toString(),
                 QLatin1String("match"));
        QByteArray bad = good;
        bad[0] = char(bad.at(0) ^ 0xFF);
        v = verifyPackage(zip, bad);
        QCOMPARE(v.value(QLatin1String("status")).toString(),
                 QLatin1String("rejected"));
        QCOMPARE(v.value(QLatin1String("expected_sha256")).toString(),
                 QLatin1String("mismatch"));
    }
    // Traversal member names reject before Qt's unzip ever sees them.
    for (const QByteArray &name : {
             QByteArrayLiteral("../../evil"),
             QByteArrayLiteral("a/../../evil"),
             QByteArrayLiteral("..\\evil"),
             QByteArrayLiteral("/abs/path"),
             QByteArrayLiteral("C:\\evil")}) {
        const QByteArray zip = buildZip({{ name, QByteArrayLiteral("x") }});
        const QJsonObject v = verifyPackage(zip);
        QCOMPARE(v.value(QLatin1String("status")).toString(),
                 QLatin1String("rejected"));
    }
    // A CRX3 with no proofs classifies "unsigned" — not "bad" — and
    // its manifest still parses.
    {
        const QByteArray pkg = buildCrx3(QByteArray(), packageZip());
        const QJsonObject v = verifyPackage(pkg);
        QCOMPARE(v.value(QLatin1String("status")).toString(),
                 QLatin1String("valid"));
        QCOMPARE(v.value(QLatin1String("format")).toString(),
                 QLatin1String("crx3"));
        QCOMPARE(v.value(QLatin1String("signing")).toString(),
                 QLatin1String("unsigned"));
        QCOMPARE(v.value(QLatin1String("manifest"))
                     .toObject().value(QLatin1String("name")).toString(),
                 QLatin1String("T"));
    }
    // A CRX3 with proof + signed_header_data classifies "signed" with
    // the honest structure-only signature check.
    {
        QByteArray proof = pbBytes(1, QByteArrayLiteral("fake-pubkey"));
        proof.append(pbBytes(2, QByteArrayLiteral("fake-sig")));
        QByteArray header = pbBytes(2, proof);
        header.append(pbBytes(5, pbBytes(1, QByteArray(16, '\x07'))));
        const QJsonObject v =
            verifyPackage(buildCrx3(header, packageZip()));
        QCOMPARE(v.value(QLatin1String("status")).toString(),
                 QLatin1String("valid"));
        QCOMPARE(v.value(QLatin1String("signing")).toString(),
                 QLatin1String("signed"));
        QCOMPARE(v.value(QLatin1String("signature_check")).toString(),
                 QLatin1String("structure-only"));
    }
    // Garbage and truncated archives reject.
    {
        QJsonObject v = verifyPackage(QByteArrayLiteral("not a package"));
        QCOMPARE(v.value(QLatin1String("status")).toString(),
                 QLatin1String("rejected"));
        const QByteArray zip = packageZip();
        v = verifyPackage(zip.left(zip.size() - 10));
        QCOMPARE(v.value(QLatin1String("status")).toString(),
                 QLatin1String("rejected"));
    }
#else
    QSKIP("rustcore not compiled in — FFI verdicts unavailable");
#endif
}

void tst_ExtVerify::manifestCheck()
{
#if defined(ARORA_RUSTCORE)
    const QByteArray doc = QByteArrayLiteral(
        "{\"manifest_version\":3,\"name\":\"D\",\"version\":\"2\","
        "\"permissions\":[\"debugger\",\"cookies\",\"storage\"],"
        "\"host_permissions\":[\"<all_urls>\",\"https://a.example/*\"],"
        "\"update_url\":\"https://u.example/x.xml\"}");
    RcBuffer out{};
    QCOMPARE(rc_ext_manifest_check(
                 reinterpret_cast<const uint8_t *>(doc.constData()),
                 size_t(doc.size()), &out), RC_OK);
    const QJsonObject check = QJsonDocument::fromJson(
        QByteArray(reinterpret_cast<const char *>(out.data),
                   qsizetype(out.len))).object();
    rc_buffer_free(out);
    QVERIFY(check.value(QLatin1String("valid")).toBool());
    const QJsonArray dangerous =
        check.value(QLatin1String("dangerous")).toArray();
    QStringList names;
    for (const QJsonValue &v : dangerous)
        names << v.toString();
    // the deny-by-default set: nativeMessaging-class APIs + <all_urls>
    QVERIFY(names.contains(QLatin1String("debugger")));
    QVERIFY(names.contains(QLatin1String("cookies")));
    QVERIFY(names.contains(QLatin1String("<all_urls>")));
    QVERIFY(!names.contains(QLatin1String("storage")));
    QVERIFY(!names.contains(QLatin1String("https://a.example/*")));
    QCOMPARE(check.value(QLatin1String("update_url")).toString(),
             QLatin1String("https://u.example/x.xml"));
    // malformed JSON refuses outright
    const QByteArray bad = QByteArrayLiteral("{");
    QCOMPARE(rc_ext_manifest_check(
                 reinterpret_cast<const uint8_t *>(bad.constData()),
                 size_t(bad.size()), &out), RC_CORRUPT);
#else
    QSKIP("rustcore not compiled in — FFI verdicts unavailable");
#endif
}

void tst_ExtVerify::inspectManifestPackages()
{
#if defined(ARORA_RUSTCORE)
    // The review dialog now sees the real manifest for .zip installs.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString zipPath = dir.path() + QLatin1String("/packed.zip");
    {
        QFile file(zipPath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(buildZip({{ QByteArrayLiteral("manifest.json"),
                QByteArrayLiteral(
                    "{\"manifest_version\":3,\"name\":\"Packed\","
                    "\"version\":\"2.0\",\"permissions\":[\"debugger\"]}"
                ) }}));
    }
    const ExtensionManager::Manifest packed =
        ExtensionManager::inspectManifest(zipPath);
    QVERIFY(packed.valid);
    QCOMPARE(packed.name, QLatin1String("Packed"));
    QVERIFY(packed.dangerous.contains(QLatin1String("debugger")));
    QVERIFY(!packed.packageNote.isEmpty());

    // A dir manifest goes through rc_ext_manifest_check too —
    // dangerous classification populated for the consent gate.
    QVERIFY(dir.isValid());
    {
        QFile file(dir.path() + QLatin1String("/manifest.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{\"manifest_version\":3,\"name\":\"DirExt\","
                   "\"permissions\":[\"nativeMessaging\"]}");
    }
    const ExtensionManager::Manifest unpacked =
        ExtensionManager::inspectManifest(dir.path());
    QVERIFY(unpacked.valid);
    QVERIFY(unpacked.dangerous.contains(QLatin1String("nativeMessaging")));

    // A rejected package surfaces its reason, not a half-parse.
    {
        QFile bad(dir.path() + QLatin1String("/evil.zip"));
        QVERIFY(bad.open(QIODevice::WriteOnly));
        bad.write(buildZip({{ QByteArrayLiteral("../../evil"),
                              QByteArrayLiteral("x") }}));
        const ExtensionManager::Manifest evil =
            ExtensionManager::inspectManifest(bad.fileName());
        QVERIFY(!evil.valid);
        QVERIFY(!evil.error.isEmpty());
    }
#else
    // No-rust builds keep the documented degradation: .zip manifests
    // stay uninspected and the dangerous list is empty.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QFile file(dir.path() + QLatin1String("/manifest.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{\"manifest_version\":3,\"name\":\"DirExt\","
                   "\"permissions\":[\"nativeMessaging\"]}");
    }
    const ExtensionManager::Manifest unpacked =
        ExtensionManager::inspectManifest(dir.path());
    QVERIFY(unpacked.valid);
    QVERIFY(unpacked.dangerous.isEmpty());
    const QString zipPath = dir.path() + QLatin1String("/packed.zip");
    {
        QFile file(zipPath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("PK\x05\x06");
    }
    QVERIFY(!ExtensionManager::inspectManifest(zipPath).valid);
#endif
}

void tst_ExtVerify::updateManifestHash()
{
    // hash_sha256 declared by the offer lands in UpdateResult so the
    // download path can hash-gate the package bytes.
    ExtensionManager::UpdateResult result;
    result.id = QLatin1String("a");
    result.currentVersion = QLatin1String("1.0");
    const QByteArray xml = QByteArrayLiteral(
        "<gupdate protocol='2.0'><app appid='a'>"
        "<updatecheck status='ok' codebase='https://x.example/p.zip' "
        "version='2.0.0' hash_sha256='0123456789abcdef0123456789abcdef"
        "0123456789abcdef0123456789abcdef'/>"
        "</app></gupdate>");
    QVERIFY(ExtensionManager::parseUpdateManifest(xml, result.id, &result));
    QVERIFY(result.error.isEmpty());
    QCOMPARE(result.availableVersion, QLatin1String("2.0.0"));
    QCOMPARE(result.packageHash,
             QLatin1String("0123456789abcdef0123456789abcdef"
                           "0123456789abcdef0123456789abcdef"));

    // An offer without a hash leaves it empty — the gate degrades to
    // structure-checks only, matching today's trust level.
    ExtensionManager::UpdateResult noHash;
    noHash.id = QLatin1String("a");
    noHash.currentVersion = QLatin1String("1.0");
    const QByteArray xmlNoHash = QByteArrayLiteral(
        "<gupdate><app appid='a'><updatecheck status='ok' "
        "codebase='https://x.example/p.zip' version='2.0.0'/>"
        "</app></gupdate>");
    QVERIFY(ExtensionManager::parseUpdateManifest(xmlNoHash, noHash.id,
                                                  &noHash));
    QVERIFY(noHash.packageHash.isEmpty());
}

QTEST_MAIN(tst_ExtVerify)
#include "tst_extverify.moc"
