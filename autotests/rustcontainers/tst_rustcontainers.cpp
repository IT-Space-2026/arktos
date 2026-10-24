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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 */

// RC19 probe: the rustcore containers module is a typed
// specialization over the profile scope's settings store — the same
// "containers" group ContainerManager persists, so a registry
// authored over the C ABI must be honored by the C++ side on the
// next scope open (the "next launch" contract), and the namespace
// derivations must match ContainerManager's paths byte-for-byte.
//
// Under ARKTOS_RUSTCORE: FFI writes land in profiles/<id>/
// settings.json — the canonical tier ProfileSettings reads first —
// so a ContainerManager constructed after the write (a fresh scope
// = a restarted process) lists the FFI-created containers, and
// rc_containers_namespace agrees with storagePath/cachePathFor/
// defaultDataPathFor.
//
// Under CONFIG+=no-rust: asserts the C++ ContainerManager keeps the
// registry alone — the fallback contract the flag exists for.

#include "qtest_arktos.h"

#include "browserpaths.h"
#include "containermanager.h"

#include <qdir.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>

#if defined(ARKTOS_RUSTCORE)
#include <rustcore.h>
#endif

class tst_RustContainers : public QObject
{
    Q_OBJECT

private slots:
#if defined(ARKTOS_RUSTCORE)
    void ffiRegistryHonoredByCpp();
    void namespaceParity();
    void cppWriteHonoredByRust();
#else
    void noRustKeepsContainerManager();
#endif
};

#if defined(ARKTOS_RUSTCORE)

namespace {

// Consume a caller-owned FFI string.
QString takeString(char *p)
{
    if (!p)
        return QString();
    const QString out = QString::fromUtf8(p);
    rc_string_free(p);
    return out;
}

// Create a container over the ABI on a fresh settings handle for
// `pid` under the app data root; returns the row's parsed JSON.
QJsonObject ffiCreate(const QString &pid, const QByteArray &spec)
{
    const QByteArray root = BrowserPaths::dataRoot().toUtf8();
    const QByteArray id = pid.toUtf8();
    RcSettings *h = rc_settings_open(root.constData(), id.constData());
    if (!h)
        return {};
    char *json = rc_containers_create(
        h, reinterpret_cast<const uint8_t *>(spec.constData()),
        static_cast<size_t>(spec.size()));
    const QString text = takeString(json);
    rc_settings_close(h);
    return QJsonDocument::fromJson(text.toUtf8()).object();
}

QJsonObject ffiNamespace(const QString &pid, const QString &cid)
{
    const QByteArray root = BrowserPaths::dataRoot().toUtf8();
    char *json = rc_containers_namespace(
        root.constData(), pid.toUtf8().constData(),
        cid.toUtf8().constData());
    return QJsonDocument::fromJson(takeString(json).toUtf8()).object();
}

} // namespace

void tst_RustContainers::ffiRegistryHonoredByCpp()
{
    // A persona id this process never opened — the C++ scope's first
    // open happens after the FFI write, which is exactly the
    // "registry written via FFI is honored on next launch" contract
    // (the shared per-scope store decodes settings.json at mint).
    const QString pid = QStringLiteral("ctffi");

    const QJsonObject a = ffiCreate(pid, "{\"name\":\"Work\",\"color\":\"#ff8800\",\"icon\":\"suitcase\"}");
    const QJsonObject b = ffiCreate(pid, "{\"name\":\"Banking\",\"color\":\"#3366ff\"}");
    QVERIFY(!a.isEmpty());
    QVERIFY(!b.isEmpty());
    const QString idA = a.value(QLatin1String("id")).toString();
    const QString idB = b.value(QLatin1String("id")).toString();
    // The C++ id shape — QUuid::toString(WithoutBraces).
    QCOMPARE(idA.size(), 36);

    // The FFI removes a container too — the C++ side honors the
    // whole registry state, not just additions.
    {
        const QByteArray root = BrowserPaths::dataRoot().toUtf8();
        RcSettings *h = rc_settings_open(root.constData(),
                                       pid.toUtf8().constData());
        QVERIFY(h);
        QCOMPARE(rc_containers_remove(h, idB.toUtf8().constData()),
                 RC_OK);
        rc_settings_close(h);
    }

    // "Next launch": the first C++ open of the scope reads the
    // FFI-authored registry.
    ContainerManager mgr(nullptr, pid);
    QCOMPARE(mgr.containers().size(), 1);
    QVERIFY(mgr.isContainerId(idA));
    QVERIFY(!mgr.isContainerId(idB));
    const ContainerManager::Container seen = mgr.containerForId(idA);
    QCOMPARE(seen.name, QLatin1String("Work"));
    QCOMPARE(seen.color, QColor(QStringLiteral("#ff8800")));

    // A C++ mutation lands back in the shared store a fresh FFI
    // handle decodes.
    QVERIFY(mgr.renameContainer(idA, QLatin1String("Renamed")));
    {
        const QByteArray root = BrowserPaths::dataRoot().toUtf8();
        RcSettings *h = rc_settings_open(root.constData(),
                                       pid.toUtf8().constData());
        QVERIFY(h);
        const QString row = takeString(
            rc_containers_get(h, idA.toUtf8().constData()));
        rc_settings_close(h);
        QCOMPARE(QJsonDocument::fromJson(row.toUtf8())
                     .object()
                     .value(QLatin1String("name"))
                     .toString(),
                 QLatin1String("Renamed"));
    }

    // C++ deletion of an FFI-created container cleans both the
    // registry and the derived storage trees — make the dirs real
    // first so the removal is observable.
    const QJsonObject ns = ffiNamespace(pid, idA);
    QVERIFY(QDir().mkpath(ns.value(QLatin1String("storage_path")).toString()));
    QVERIFY(QDir().mkpath(ns.value(QLatin1String("cache_path")).toString()));
    QVERIFY(mgr.deleteContainer(idA));
    QVERIFY(!mgr.isContainerId(idA));
    QVERIFY(!QDir(ns.value(QLatin1String("storage_path")).toString()).exists());
    QVERIFY(!QDir(ns.value(QLatin1String("cache_path")).toString()).exists());
}

void tst_RustContainers::namespaceParity()
{
    const QString pid = QStringLiteral("ctns");
    const QJsonObject made =
        ffiCreate(pid, "{\"name\":\"Ns\",\"color\":\"#000000\"}");
    QVERIFY(!made.isEmpty());
    const QString id = made.value(QLatin1String("id")).toString();

    ContainerManager mgr(nullptr, pid);
    QVERIFY(mgr.isContainerId(id));

    const QJsonObject ns = ffiNamespace(pid, id);
    QCOMPARE(ns.value(QLatin1String("storage_path")).toString(),
             mgr.storagePath(id));
    QCOMPARE(ns.value(QLatin1String("cache_path")).toString(),
             mgr.cachePathFor(id));
    QCOMPARE(ns.value(QLatin1String("default_data_path")).toString(),
             mgr.defaultDataPathFor(id));
    QCOMPARE(ns.value(QLatin1String("storage_name")).toString(),
             QStringLiteral("arktos-container-%1-%2").arg(pid, id));
    // RC15's cookie partition is the container id itself.
    QCOMPARE(ns.value(QLatin1String("cookie_partition")).toString(), id);
}

void tst_RustContainers::cppWriteHonoredByRust()
{
    const QString pid = QStringLiteral("ctcpp");
    QString id;
    {
        ContainerManager mgr(nullptr, pid);
        const ContainerManager::Container c =
            mgr.createContainer(QLatin1String("FromCpp"),
                                QColor(QStringLiteral("#123456")));
        id = c.id;
        QVERIFY(mgr.isContainerId(id));
    }

    // A fresh ABI read sees the C++-authored row (ProfileSettings
    // writes through to the canonical settings.json).
    const QByteArray root = BrowserPaths::dataRoot().toUtf8();
    RcSettings *h = rc_settings_open(root.constData(),
                                   pid.toUtf8().constData());
    QVERIFY(h);
    const QString list = takeString(rc_containers_list(h));
    const QJsonArray rows = QJsonDocument::fromJson(list.toUtf8()).array();
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows.first().toObject().value(QLatin1String("id")).toString(),
             id);
    QCOMPARE(rows.first().toObject().value(QLatin1String("name")).toString(),
             QLatin1String("FromCpp"));
    QCOMPARE(rows.first().toObject().value(QLatin1String("color")).toString(),
             QLatin1String("#123456"));
    rc_settings_close(h);
}

#else // !ARKTOS_RUSTCORE — the fallback contract

void tst_RustContainers::noRustKeepsContainerManager()
{
    const QString pid = QStringLiteral("ctnr");
    ContainerManager mgr(nullptr, pid);
    const ContainerManager::Container c =
        mgr.createContainer(QLatin1String("Legacy"),
                            QColor(QStringLiteral("#112233")));
    QVERIFY(mgr.isContainerId(c.id));
    QCOMPARE(mgr.containerForId(c.id).name, QLatin1String("Legacy"));
    QVERIFY(mgr.deleteContainer(c.id));
    QVERIFY(!mgr.isContainerId(c.id));
}

#endif

QTEST_MAIN(tst_RustContainers)
#include "tst_rustcontainers.moc"
