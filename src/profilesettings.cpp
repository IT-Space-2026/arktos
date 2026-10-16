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

#include "profilesettings.h"

#include <qcoreapplication.h>
#include <qdir.h>
#include <qfile.h>
#include <qstandardpaths.h>

QString ProfilePaths::dirFor(const QString &id)
{
    QString directory =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (directory.isEmpty())
        directory = QDir::homePath() + QLatin1String("/.")
            + QCoreApplication::applicationName();
    const QString path =
        directory + QLatin1String("/profiles/") + id;
    if (!QFile::exists(path))
        QDir().mkpath(path);
    return path;
}

namespace {

// MPROF04 audit (.devin/MPROF04-settings.md) — every top-level group
// whose keys are persona state.  A group names a whole subtree;
// container-style children (containers/<id>/..., webpermissions/<origin>,
// extensions/integrity/<storage>/<ext>) classify with their root.
const char *const kProfileGroups[] = {
    "AdBlock",
    "BrowserMainWindow",
    "LanguageManager",
    "MainWindow",
    "Settings",
    "SourceViewer",
    "autofill",
    "browser",
    "clearprivatedata",
    "commandPalette",
    "containers",
    "cookies",
    "downloadmanager",
    "extensions",
    "history",
    "navbar",
    "network",
    "openSearch",
    "performance",
    "popupExceptions",
    "privacy",
    "profile",
    "proxy",
    "reader",
    "scriptcontrol",
    "searchShortcuts",
    "sessions",
    "sidebar",
    "tabs",
    "toolbarsearch",
    "trust",
    "urlcleaner",
    "urlloading",
    "webpermissions",
    "websettings",
};

// Persona keys that sit at the store root (no group).
const char *const kProfileKeys[] = {
    "userAgent",
};

// App-level keys inside otherwise-profile groups — they keep living on
// the global file (crash guards, engine defaults, shared trust state,
// machine-global list metadata).
const char *const kGlobalExceptions[] = {
    "MainWindow/defaultEngine",
    "MainWindow/restoring",
    "browser/gpuCrashCount",
    "browser/lastExit",
    "browser/sessionActive",
    "browser/sessionActiveTor",
    "network/CaCertificates",
    "privacy/trancoListUpdated",
    "privacy/domainBlocklistUpdated",
};

} // namespace

ProfileSettings::ProfileSettings()
    : ProfileSettings(currentId())
{
}

ProfileSettings::ProfileSettings(const QString &profileId)
    : m_id(profileId)
    , m_profile(fileFor(profileId), QSettings::IniFormat)
    , m_global()
    , m_arrayStore(nullptr)
{
}

QString ProfileSettings::profileId() const
{
    return m_id;
}

QString ProfileSettings::defaultId()
{
    return QStringLiteral("default");
}

QString ProfileSettings::currentId()
{
    // MPROF03 retargets this: windows bind to a profile id, profile-
    // affine readers resolve the window's id here.  Until then every
    // context resolves to the single profile that exists.
    return defaultId();
}

QString ProfileSettings::fileFor(const QString &id)
{
    return ProfilePaths::dirFor(id)
        + QLatin1String("/settings.ini");
}

bool ProfileSettings::isProfileKey(const QString &path)
{
    for (const char *exception : kGlobalExceptions) {
        if (path == QLatin1String(exception))
            return false;
    }
    const QString root = path.section(QLatin1Char('/'), 0, 0);
    for (const char *group : kProfileGroups) {
        if (root == QLatin1String(group))
            return true;
    }
    for (const char *key : kProfileKeys) {
        if (path == QLatin1String(key))
            return true;
    }
    return false;
}

void ProfileSettings::migrateFromGlobal(const QString &id)
{
    QSettings dst(fileFor(id), QSettings::IniFormat);
    const QLatin1String marker("meta/settingsMigrated");
    if (dst.value(marker).toBool())
        return;
    QSettings src;
    const QStringList keys = src.allKeys();
    for (const QString &key : keys) {
        if (!isProfileKey(key) || dst.contains(key))
            continue;
        dst.setValue(key, src.value(key));
    }
    dst.setValue(marker, true);
    dst.sync();
}

QString ProfileSettings::keyPath(const QString &key) const
{
    const QString group = m_profile.group();
    if (group.isEmpty())
        return key;
    if (key.isEmpty())
        return group;
    return group + QLatin1Char('/') + key;
}

QSettings &ProfileSettings::storeForPath(const QString &path)
{
    return isProfileKey(path) ? m_profile : m_global;
}

QSettings &ProfileSettings::storeFor(const QString &key)
{
    return storeForPath(keyPath(key));
}

const QSettings &ProfileSettings::storeFor(const QString &key) const
{
    return isProfileKey(keyPath(key)) ? m_profile : m_global;
}

QVariant ProfileSettings::value(const QString &key,
                                const QVariant &defaultValue) const
{
    if (isProfileKey(keyPath(key)) && m_profile.contains(key))
        return m_profile.value(key, defaultValue);
    return m_global.value(key, defaultValue);
}

void ProfileSettings::setValue(const QString &key, const QVariant &value)
{
    storeFor(key).setValue(key, value);
}

void ProfileSettings::remove(const QString &key)
{
    if (!isProfileKey(keyPath(key))) {
        m_global.remove(key);
        return;
    }
    // A profile-store-only remove would resurrect the legacy copy
    // through the read fallback — drop both.
    m_profile.remove(key);
    m_global.remove(key);
}

bool ProfileSettings::contains(const QString &key) const
{
    if (!isProfileKey(keyPath(key)))
        return m_global.contains(key);
    return m_profile.contains(key) || m_global.contains(key);
}

void ProfileSettings::beginGroup(const QString &prefix)
{
    m_profile.beginGroup(prefix);
    m_global.beginGroup(prefix);
}

void ProfileSettings::endGroup()
{
    m_profile.endGroup();
    m_global.endGroup();
}

QString ProfileSettings::group() const
{
    return m_profile.group();
}

QStringList ProfileSettings::childKeys() const
{
    QStringList keys = m_global.childKeys();
    if (isProfileKey(group())) {
        for (const QString &key : m_profile.childKeys()) {
            if (!keys.contains(key))
                keys.append(key);
        }
        keys.sort();
    }
    return keys;
}

QStringList ProfileSettings::childGroups() const
{
    QStringList groups = m_global.childGroups();
    if (isProfileKey(group())) {
        for (const QString &child : m_profile.childGroups()) {
            if (!groups.contains(child))
                groups.append(child);
        }
        groups.sort();
    }
    return groups;
}

QStringList ProfileSettings::allKeys() const
{
    QStringList keys = m_global.allKeys();
    for (const QString &key : m_profile.allKeys()) {
        if (!keys.contains(key))
            keys.append(key);
    }
    keys.sort();
    return keys;
}

int ProfileSettings::beginReadArray(const QString &prefix)
{
    if (!isProfileKey(keyPath(prefix))) {
        m_arrayStore = &m_global;
    } else {
        // The profile store is always IniFormat, where an array lands
        // a "<prefix>/size" key — contains() tells a stored (even
        // empty) array from an absent one; absent defers to the
        // legacy global file.
        m_arrayStore =
            m_profile.contains(prefix + QLatin1String("/size"))
                ? &m_profile : &m_global;
    }
    return m_arrayStore->beginReadArray(prefix);
}

void ProfileSettings::beginWriteArray(const QString &prefix, int size)
{
    m_arrayStore = &storeFor(prefix);
    m_arrayStore->beginWriteArray(prefix, size);
}

void ProfileSettings::setArrayIndex(int i)
{
    if (m_arrayStore)
        m_arrayStore->setArrayIndex(i);
}

void ProfileSettings::endArray()
{
    if (m_arrayStore)
        m_arrayStore->endArray();
    m_arrayStore = nullptr;
}

void ProfileSettings::clear()
{
    // Blank-slate reset matching the pre-split QSettings::clear()
    // contract: the profile file AND the global file.  Tests use this
    // to reset state — a profile-file-only wipe would leave the read
    // fallback resurrecting classified globals.
    m_profile.clear();
    m_global.clear();
}

void ProfileSettings::sync()
{
    m_profile.sync();
    m_global.sync();
}

QSettings::Status ProfileSettings::status() const
{
    return m_profile.status() != QSettings::NoError
        ? m_profile.status() : m_global.status();
}

QString ProfileSettings::fileName() const
{
    return m_profile.fileName();
}

bool ProfileSettings::isWritable() const
{
    return m_profile.isWritable() && m_global.isWritable();
}
