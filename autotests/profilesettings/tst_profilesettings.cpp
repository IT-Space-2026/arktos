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

#include <profilesettings.h>

#include <qsettings.h>
#include <qtemporarydir.h>

// MPROF04: the per-profile settings split — isolation between profile
// inis, the global->default copy migration, the global-key exceptions,
// and the legacy-file read fallback that keeps pre-migration values
// (and test seeds) visible.
class tst_ProfileSettings : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void layout();
    void classification();
    void profileWriteDoesNotLeak();
    void migrationPopulatesProfileIni();
    void migrationIsOneShot();
    void globalKeysStayGlobal();
    void legacyFallbackRead();
    void removeDropsBothStores();
    void groupRouting();
    void writeShadowsFallback();
};

void tst_ProfileSettings::initTestCase()
{
    QSettings::setDefaultFormat(QSettings::IniFormat);
}

void tst_ProfileSettings::layout()
{
    QCOMPARE(ProfileSettings::currentId(),
             QStringLiteral("default"));
    const QString dir = ProfilePaths::dirFor(QStringLiteral("layoutid"));
    QVERIFY(dir.endsWith(QLatin1String("/profiles/layoutid")));
    QVERIFY(QDir(dir).exists());
    QCOMPARE(ProfileSettings::fileFor(QStringLiteral("layoutid")),
             dir + QLatin1String("/settings.ini"));
}

void tst_ProfileSettings::classification()
{
    QVERIFY(ProfileSettings::isProfileKey(
        QLatin1String("privacy/securityLevel")));
    QVERIFY(ProfileSettings::isProfileKey(
        QLatin1String("MainWindow/home")));
    QVERIFY(ProfileSettings::isProfileKey(
        QLatin1String("downloadmanager/downloadDirectory")));
    QVERIFY(ProfileSettings::isProfileKey(
        QLatin1String("userAgent")));
    // Global exceptions inside profile groups.
    QVERIFY(!ProfileSettings::isProfileKey(
        QLatin1String("MainWindow/defaultEngine")));
    QVERIFY(!ProfileSettings::isProfileKey(
        QLatin1String("MainWindow/restoring")));
    QVERIFY(!ProfileSettings::isProfileKey(
        QLatin1String("browser/gpuCrashCount")));
    QVERIFY(!ProfileSettings::isProfileKey(
        QLatin1String("browser/sessionActive")));
    QVERIFY(!ProfileSettings::isProfileKey(
        QLatin1String("network/CaCertificates")));
    // Never-classified groups.
    QVERIFY(!ProfileSettings::isProfileKey(
        QLatin1String("sandbox/enabled")));
    QVERIFY(!ProfileSettings::isProfileKey(
        QLatin1String("anything/atAll")));
}

void tst_ProfileSettings::profileWriteDoesNotLeak()
{
    // Profile B's privacy toggle must land in B's ini only — not in
    // A's ini and not in the shared global file.
    ProfileSettings a(QStringLiteral("leakA"));
    ProfileSettings b(QStringLiteral("leakB"));
    b.setValue(QLatin1String("privacy/blockPings"), false);
    b.sync();

    QCOMPARE(a.value(QLatin1String("privacy/blockPings"), true).toBool(),
             true);

    QSettings iniA(ProfileSettings::fileFor(QStringLiteral("leakA")),
                   QSettings::IniFormat);
    QVERIFY(!iniA.contains(QLatin1String("privacy/blockPings")));
    QSettings iniB(ProfileSettings::fileFor(QStringLiteral("leakB")),
                   QSettings::IniFormat);
    QCOMPARE(iniB.value(QLatin1String("privacy/blockPings")).toBool(),
             false);
    // The write never touched the global file.
    QVERIFY(!QSettings().contains(QLatin1String("privacy/blockPings")));
}

void tst_ProfileSettings::migrationPopulatesProfileIni()
{
    // A pre-split install: persona keys exist only in the global file.
    const QString id = QStringLiteral("migA");
    QSettings().setValue(QLatin1String("privacy/securityLevel"), 2);
    QSettings().setValue(QLatin1String("MainWindow/home"),
                         QStringLiteral("https://example.test/"));
    QSettings().sync();

    ProfileSettings::migrateFromGlobal(id);

    QSettings ini(ProfileSettings::fileFor(id), QSettings::IniFormat);
    QCOMPARE(ini.value(QLatin1String("privacy/securityLevel")).toInt(), 2);
    QCOMPARE(ini.value(QLatin1String("MainWindow/home")).toString(),
             QStringLiteral("https://example.test/"));
    QCOMPARE(ini.value(QLatin1String("meta/settingsMigrated")).toBool(),
             true);
    // Copy-not-move: the global file kept its copies.
    QCOMPARE(QSettings()
                 .value(QLatin1String("privacy/securityLevel")).toInt(),
             2);
}

void tst_ProfileSettings::migrationIsOneShot()
{
    const QString id = QStringLiteral("migB");
    ProfileSettings::migrateFromGlobal(id);
    ProfileSettings settings(id);
    settings.setValue(QLatin1String("privacy/securityLevel"), 0);
    settings.sync();

    // A second run is a no-op — the profile's own edit survives.
    ProfileSettings::migrateFromGlobal(id);
    QCOMPARE(settings.value(QLatin1String("privacy/securityLevel"))
                 .toInt(), 0);
}

void tst_ProfileSettings::globalKeysStayGlobal()
{
    ProfileSettings settings(QStringLiteral("globA"));
    settings.setValue(QLatin1String("MainWindow/defaultEngine"),
                      QStringLiteral("servo"));
    settings.sync();

    // Readable through the plain global QSettings…
    QCOMPARE(QSettings()
                 .value(QLatin1String("MainWindow/defaultEngine"))
                 .toString(),
             QStringLiteral("servo"));
    // …and absent from the profile ini.
    QSettings ini(ProfileSettings::fileFor(QStringLiteral("globA")),
                  QSettings::IniFormat);
    QVERIFY(!ini.contains(QLatin1String("MainWindow/defaultEngine")));
    QSettings().remove(QLatin1String("MainWindow/defaultEngine"));
}

void tst_ProfileSettings::legacyFallbackRead()
{
    // A value that only exists in the legacy global file is still
    // seen — pre-migration state and fixture seeds keep working.
    const QString id = QStringLiteral("fbA");
    QSettings().setValue(QLatin1String("privacy/clearOnExit"), true);
    QSettings().sync();

    ProfileSettings settings(id);
    QCOMPARE(settings.value(QLatin1String("privacy/clearOnExit"))
                 .toBool(), true);
    QSettings().remove(QLatin1String("privacy/clearOnExit"));
}

void tst_ProfileSettings::removeDropsBothStores()
{
    const QString id = QStringLiteral("rmA");
    QSettings().setValue(QLatin1String("privacy/dnsPrefetch"), true);
    QSettings().sync();

    ProfileSettings settings(id);
    settings.remove(QLatin1String("privacy/dnsPrefetch"));
    settings.sync();

    QVERIFY(!settings.contains(QLatin1String("privacy/dnsPrefetch")));
    QVERIFY(!QSettings().contains(QLatin1String("privacy/dnsPrefetch")));
}

void tst_ProfileSettings::groupRouting()
{
    ProfileSettings settings(QStringLiteral("grpA"));
    settings.beginGroup(QLatin1String("privacy"));
    settings.setValue(QLatin1String("blockRemoteFonts"), true);
    settings.endGroup();
    settings.sync();

    QSettings ini(ProfileSettings::fileFor(QStringLiteral("grpA")),
                  QSettings::IniFormat);
    QCOMPARE(ini.value(QLatin1String("privacy/blockRemoteFonts"))
                 .toBool(), true);
    QVERIFY(!QSettings().contains(QLatin1String("privacy/blockRemoteFonts")));
}

void tst_ProfileSettings::writeShadowsFallback()
{
    const QString id = QStringLiteral("fbB");
    QSettings().setValue(QLatin1String("history/historyLimit"), 30);
    QSettings().sync();

    ProfileSettings settings(id);
    QCOMPARE(settings.value(QLatin1String("history/historyLimit"))
                 .toInt(), 30);

    // Once the profile store has its own copy it wins.
    settings.setValue(QLatin1String("history/historyLimit"), 7);
    QCOMPARE(settings.value(QLatin1String("history/historyLimit"))
                 .toInt(), 7);
    QSettings().remove(QLatin1String("history/historyLimit"));
}

QTEST_MAIN(tst_ProfileSettings)
#include "tst_profilesettings.moc"
