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

#include "profilesettings.h"

#include "browserpaths.h"

#ifdef ARKTOS_RUSTCORE
#include "rustcorebridge.h"

#include <qhash.h>
#endif

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
    "privacy/majesticListUpdated",
    "privacy/domainBlocklistUpdated",
};

#ifdef ARKTOS_RUSTCORE
// The conf-cache analogue: every ProfileSettings for a scope shares
// one store handle — RcSettings owns a decoded document per handle,
// so per-object handles would diverge the way unshared conf files
// would.  The mint goes through the shared AcCore when the bridge
// serves (RC08); RustCoreSettings falls back to rc_settings_open
// itself when it cannot.
QHash<QString, RustCoreSettings *> &rustStores()
{
    static QHash<QString, RustCoreSettings *> stores;
    return stores;
}

// The app's data root can move after early construction (the test
// harness renames the application post-BrowserApplication).  Stores
// minted before the move stay pinned to the old scope — retire them
// (kept alive so in-flight ProfileSettings pointers stay valid) and
// let the next call remint against the current root.
QString &rustStoresRoot()
{
    static QString root = BrowserPaths::dataRoot();
    return root;
}

void retireStoresOnDrift()
{
    const QString root = BrowserPaths::dataRoot();
    if (root == rustStoresRoot())
        return;
    static QList<RustCoreSettings *> retired;
    retired += rustStores().values();
    rustStores().clear();
    rustStoresRoot() = root;
}

// Empty profileId selects the global scope, same as the ctor.
RustCoreSettings *rustStoreFor(const QString &profileId)
{
    retireStoresOnDrift();
    RustCoreSettings *&store = rustStores()[profileId];
    if (!store)
        store = new RustCoreSettings(profileId);
    return store;
}

// Group-aware remove for the flat store — allKeys' boundary match
// covers the exact key and any "path/…" subtree, matching
// QSettings::remove on a group.
void rustRemove(RustCoreSettings *store, const QString &path)
{
    if (!store)
        return;
    const QStringList keys = store->allKeys(path);
    for (const QString &key : keys)
        store->remove(key);
}
#endif

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
#ifdef ARKTOS_RUSTCORE
    , m_profileStore(rustStoreFor(profileId))
    , m_globalStore(rustStoreFor(QString()))
    , m_flatStore(nullptr)
#endif
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
    // MPROF03: the ambient persona is window-bound — BrowserMainWindow
    // pushes its profile id on construction and activation, so a
    // context-less ProfileSettings() resolves the focused window's
    // persona.  Readers that own their context (panels, stores) bind
    // an explicit id instead.
    return BrowserPaths::activeProfileId();
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
    // One-shot copy (not move): classified keys land in the profile
    // ini — which under rustcore the profile scope's auto-import
    // folds into settings.json on first open — and the global ini
    // merge runs through the RC02 explicit-import lanes on both
    // scopes (their imported_extra markers make re-runs no-ops).
    QSettings dst(fileFor(id), QSettings::IniFormat);
    const QLatin1String marker("meta/settingsMigrated");
    if (!dst.value(marker).toBool()) {
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
#ifdef ARKTOS_RUSTCORE
    // Profile scope: the auto-import above may already have claimed
    // the ini this call wrote; the explicit lane then folds any
    // classified stragglers straight from the global file.  Global
    // scope: the complementary global-key set.
    const QString ini = QSettings().fileName();
    rustStoreFor(id)->importLegacyIni(ini);
    rustStoreFor(QString())->importLegacyIni(ini);
#endif
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

#ifdef ARKTOS_RUSTCORE
QString ProfileSettings::flatArrayKey(const QString &key) const
{
    // The ini wire numbers array elements 1-based.
    return m_arrayPath + QLatin1Char('/')
        + QString::number(m_arrayIndex + 1) + QLatin1Char('/') + key;
}

QStringList ProfileSettings::rustChildren(bool groups) const
{
    QStringList out;
    const QString grp = group();
    const QString base =
        grp.isEmpty() ? QString() : grp + QLatin1Char('/');
    for (RustCoreSettings *store : {m_profileStore, m_globalStore}) {
        if (!store)
            continue;
        const QStringList keys = store->allKeys(grp);
        for (const QString &full : keys) {
            const QString rest = full.mid(base.size());
            if (rest.isEmpty())
                continue;
            const int slash = rest.indexOf(QLatin1Char('/'));
            if (groups != (slash >= 0))
                continue;
            const QString name = groups ? rest.left(slash) : rest;
            if (!out.contains(name))
                out.append(name);
        }
    }
    return out;
}
#endif

QVariant ProfileSettings::value(const QString &key,
                                const QVariant &defaultValue) const
{
#ifdef ARKTOS_RUSTCORE
    if (m_arrayStore) {
        if (m_flatStore) {
            const QString flat = flatArrayKey(key);
            if (m_flatStore->contains(flat))
                return m_flatStore->value(flat, defaultValue);
        }
        return m_arrayStore->value(key, defaultValue);
    }
#endif
    const QString path = keyPath(key);
    if (isProfileKey(path)) {
#ifdef ARKTOS_RUSTCORE
        // Canonical forward store, then the compat mirror, then the
        // legacy global file (the copy-not-move fallback).
        if (m_profileStore && m_profileStore->contains(path))
            return m_profileStore->value(path, defaultValue);
        if (m_profile.contains(key))
            return m_profile.value(key, defaultValue);
        return m_global.value(key, defaultValue);
#else
        if (m_profile.contains(key))
            return m_profile.value(key, defaultValue);
#endif
    }
#ifdef ARKTOS_RUSTCORE
    // Global keys: the ini stays authoritative in-process — raw
    // QSettings writers RC08 leaves alone keep updating it — with
    // the mirrored json as the last tier before the default.
    if (m_global.contains(key))
        return m_global.value(key, defaultValue);
    if (m_globalStore && m_globalStore->contains(path))
        return m_globalStore->value(path, defaultValue);
#endif
    return m_global.value(key, defaultValue);
}

void ProfileSettings::setValue(const QString &key, const QVariant &value)
{
    if (m_arrayStore) {
        m_arrayStore->setValue(key, value);
#ifdef ARKTOS_RUSTCORE
        if (m_flatStore)
            m_flatStore->setValue(flatArrayKey(key), value);
#endif
        return;
    }
    const QString path = keyPath(key);
    if (!isProfileKey(path)) {
        m_global.setValue(key, value);
#ifdef ARKTOS_RUSTCORE
        if (m_globalStore)
            m_globalStore->setValue(path, value);
#endif
        return;
    }
    m_profile.setValue(key, value);
#ifdef ARKTOS_RUSTCORE
    if (m_profileStore)
        m_profileStore->setValue(path, value);
#endif
}

void ProfileSettings::remove(const QString &key)
{
    if (m_arrayStore) {
        m_arrayStore->remove(key);
#ifdef ARKTOS_RUSTCORE
        if (m_flatStore)
            m_flatStore->remove(flatArrayKey(key));
#endif
        return;
    }
    const QString path = keyPath(key);
    if (!isProfileKey(path)) {
        m_global.remove(key);
#ifdef ARKTOS_RUSTCORE
        rustRemove(m_globalStore, path);
#endif
        return;
    }
    // A profile-store-only remove would resurrect the legacy copy
    // through the read fallback — drop both.
    m_profile.remove(key);
    m_global.remove(key);
#ifdef ARKTOS_RUSTCORE
    rustRemove(m_profileStore, path);
    rustRemove(m_globalStore, path);
#endif
}

bool ProfileSettings::contains(const QString &key) const
{
    if (m_arrayStore) {
#ifdef ARKTOS_RUSTCORE
        if (m_flatStore && m_flatStore->contains(flatArrayKey(key)))
            return true;
#endif
        return m_arrayStore->contains(key);
    }
    const QString path = keyPath(key);
    if (!isProfileKey(path)) {
#ifdef ARKTOS_RUSTCORE
        if (m_global.contains(key))
            return true;
        return m_globalStore && m_globalStore->contains(path);
#else
        return m_global.contains(key);
#endif
    }
    if (m_profile.contains(key) || m_global.contains(key))
        return true;
#ifdef ARKTOS_RUSTCORE
    return m_profileStore && m_profileStore->contains(path);
#else
    return false;
#endif
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
    bool merged = isProfileKey(group());
    if (merged) {
        for (const QString &key : m_profile.childKeys()) {
            if (!keys.contains(key))
                keys.append(key);
        }
    }
#ifdef ARKTOS_RUSTCORE
    for (const QString &key : rustChildren(false)) {
        if (!keys.contains(key)) {
            keys.append(key);
            merged = true;
        }
    }
#endif
    if (merged)
        keys.sort();
    return keys;
}

QStringList ProfileSettings::childGroups() const
{
    QStringList groups = m_global.childGroups();
    bool merged = isProfileKey(group());
    if (merged) {
        for (const QString &child : m_profile.childGroups()) {
            if (!groups.contains(child))
                groups.append(child);
        }
    }
#ifdef ARKTOS_RUSTCORE
    for (const QString &child : rustChildren(true)) {
        if (!groups.contains(child)) {
            groups.append(child);
            merged = true;
        }
    }
#endif
    if (merged)
        groups.sort();
    return groups;
}

QStringList ProfileSettings::allKeys() const
{
    QStringList keys = m_global.allKeys();
    for (const QString &key : m_profile.allKeys()) {
        if (!keys.contains(key))
            keys.append(key);
    }
#ifdef ARKTOS_RUSTCORE
    const QString grp = group();
    const QString base =
        grp.isEmpty() ? QString() : grp + QLatin1Char('/');
    for (RustCoreSettings *store : {m_profileStore, m_globalStore}) {
        if (!store)
            continue;
        for (const QString &full : store->allKeys(grp)) {
            const QString rel = full.mid(base.size());
            if (!rel.isEmpty() && !keys.contains(rel))
                keys.append(rel);
        }
    }
#endif
    keys.sort();
    return keys;
}

int ProfileSettings::beginReadArray(const QString &prefix)
{
    const QString path = keyPath(prefix);
#ifdef ARKTOS_RUSTCORE
    m_arrayPath = path;
    m_arrayIndex = -1;
    m_arraySize = -1;
    m_arrayMax = -1;
    m_arrayWriting = false;
    m_flatStore = nullptr;
#endif
    if (!isProfileKey(path)) {
        m_arrayStore = &m_global;
#ifdef ARKTOS_RUSTCORE
        // A facade-side global-scope write can exist only in the
        // json — prefer it when the ini lacks the array.
        if (m_globalStore
                && !m_global.contains(prefix + QLatin1String("/size"))
                && m_globalStore->contains(path + QLatin1String("/size")))
            m_flatStore = m_globalStore;
#endif
    } else {
#ifdef ARKTOS_RUSTCORE
        if (m_profileStore
                && m_profileStore->contains(path + QLatin1String("/size"))) {
            // The canonical tier has the array — reads serve flat;
            // the ini mirror still opens so element edits land in
            // both.
            m_flatStore = m_profileStore;
            m_arrayStore = &m_profile;
        } else
#endif
        {
            // The profile store is always IniFormat, where an array
            // lands a "<prefix>/size" key — contains() tells a stored
            // (even empty) array from an absent one; absent defers to
            // the legacy global file.
            m_arrayStore =
                m_profile.contains(prefix + QLatin1String("/size"))
                    ? &m_profile : &m_global;
        }
    }
    const int size = m_arrayStore->beginReadArray(prefix);
#ifdef ARKTOS_RUSTCORE
    if (m_flatStore)
        return m_flatStore->value(path + QLatin1String("/size")).toInt();
#endif
    return size;
}

void ProfileSettings::beginWriteArray(const QString &prefix, int size)
{
    const QString path = keyPath(prefix);
    m_arrayStore = isProfileKey(path) ? &m_profile : &m_global;
#ifdef ARKTOS_RUSTCORE
    m_arrayPath = path;
    m_arrayIndex = -1;
    m_arraySize = size;
    m_arrayMax = -1;
    m_arrayWriting = true;
    m_flatStore = isProfileKey(path) ? m_profileStore : m_globalStore;
    if (m_flatStore && size >= 0)
        m_flatStore->setValue(path + QLatin1String("/size"), size);
#endif
    m_arrayStore->beginWriteArray(prefix, size);
}

void ProfileSettings::setArrayIndex(int i)
{
#ifdef ARKTOS_RUSTCORE
    m_arrayIndex = i;
    if (i > m_arrayMax)
        m_arrayMax = i;
#endif
    if (m_arrayStore)
        m_arrayStore->setArrayIndex(i);
}

void ProfileSettings::endArray()
{
    if (m_arrayStore)
        m_arrayStore->endArray();
    m_arrayStore = nullptr;
#ifdef ARKTOS_RUSTCORE
    // A write opened with size -1 resolves the count at close — the
    // ini just wrote it; mirror the same number into the json.
    if (m_flatStore && m_arrayWriting && m_arraySize < 0
            && m_arrayMax >= 0) {
        m_flatStore->setValue(m_arrayPath + QLatin1String("/size"),
                              m_arrayMax + 1);
    }
    m_flatStore = nullptr;
#endif
}

void ProfileSettings::clear()
{
    // Blank-slate reset matching the pre-split QSettings::clear()
    // contract: the profile file AND the global file — plus their
    // rust scopes under RC08.  Tests use this to reset state — a
    // profile-file-only wipe would leave the read fallback
    // resurrecting classified globals.
    m_profile.clear();
    m_global.clear();
#ifdef ARKTOS_RUSTCORE
    if (m_profileStore)
        m_profileStore->clear();
    if (m_globalStore)
        m_globalStore->clear();
#endif
}

void ProfileSettings::sync()
{
    m_profile.sync();
    m_global.sync();
#ifdef ARKTOS_RUSTCORE
    if (m_profileStore)
        m_profileStore->sync();
    if (m_globalStore)
        m_globalStore->sync();
#endif
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
