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

#ifndef PROFILESETTINGS_H
#define PROFILESETTINGS_H

#include <qsettings.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qvariant.h>

// MPROF04 standalone fallback for MPROF01's layout contract — resolves
// AppDataLocation/profiles/<id>/.  MPROF01 lands the canonical helper
// (profileDataFilePath/ProfileRegistry); whoever merges second deletes
// the duplicate.  Do NOT invent a different layout.
namespace ProfilePaths {

// <app data dir>/profiles/<id> — created on demand.
QString dirFor(const QString &id);

}

// MPROF04: per-profile settings store.  Persona keys live in
// profiles/<id>/settings.ini (IniFormat); app-global keys stay on the
// plain QSettings file.  This is a QSettings-compatible facade over
// BOTH stores that routes each key by full path — the audit in
// .devin/MPROF04-settings.md is the classification isProfileKey()
// implements.
//
//   - classified key  -> reads: profile store first, then the global
//                        file (the copy-not-move migration's "old keys
//                        stay as harmless fallback" half); writes:
//                        profile store only.
//   - global key      -> the plain QSettings file, unchanged.
//
// Each instance owns its two QSettings objects — never share one
// object across call sites: beginGroup() mutates its group stack, and
// a site that forgets endGroup() would poison a shared object's paths.
// Objects on the same file share Qt's in-process conf cache, so
// construction is cheap and writes are immediately visible.
class ProfileSettings
{
public:
    // Binds currentId().
    ProfileSettings();
    explicit ProfileSettings(const QString &profileId);

    // The QSettings subset the tree uses — same call shapes.
    QVariant value(const QString &key,
                   const QVariant &defaultValue = QVariant()) const;
    void setValue(const QString &key, const QVariant &value);
    void remove(const QString &key);
    bool contains(const QString &key) const;
    void beginGroup(const QString &prefix);
    void endGroup();
    QString group() const;
    QStringList childKeys() const;
    QStringList childGroups() const;
    QStringList allKeys() const;
    int beginReadArray(const QString &prefix);
    void beginWriteArray(const QString &prefix, int size = -1);
    void setArrayIndex(int i);
    void endArray();
    // Wipes BOTH stores — the pre-split single-file blank slate.
    void clear();
    void sync();
    QSettings::Status status() const;
    QString fileName() const;
    bool isWritable() const;

    QString profileId() const;

    // The id of the only profile guaranteed to exist.
    static QString defaultId();
    // Resolves the calling context's profile — "default" until MPROF03
    // lands window->profile binding; the SettingsDialog (and any other
    // profile-affine reader) binds this at construction.
    static QString currentId();

    // profiles/<id>/settings.ini (parent dir created on demand).
    static QString fileFor(const QString &id);
    // The audit codified: is this full key path persona-owned?
    static bool isProfileKey(const QString &path);

    // One-shot copy (not move) of every classified key from the global
    // file into profiles/<id>/settings.ini — a meta/settingsMigrated
    // marker inside the profile ini makes re-runs no-ops.  Called once
    // from the BrowserApplication ctor; id defaults to the single
    // profile that exists pre-MPROF01.
    static void migrateFromGlobal(const QString &id = defaultId());

private:
    QString keyPath(const QString &key) const;
    QSettings &storeFor(const QString &key);
    const QSettings &storeFor(const QString &key) const;
    QSettings &storeForPath(const QString &path);

    QString m_id;
    QSettings m_profile;
    QSettings m_global;
    // Which store the open array sequence came from.
    QSettings *m_arrayStore;
};

#endif // PROFILESETTINGS_H
