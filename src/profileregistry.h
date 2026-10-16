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

#ifndef PROFILEREGISTRY_H
#define PROFILEREGISTRY_H

#include <profilecontext.h>

#include <qobject.h>
#include <qstring.h>

// MPROF01: single source of truth for the browser's profile list.
// Backed by <data root>/profiles.json — a flat records array; the
// registry always describes at least the "default" profile (a missing
// or corrupt file synthesizes one).  ProfileIdentity delegates the
// active profile's name/avatar here so the PROF01/02 UI keeps working
// unchanged now that MPROF03 binds windows to real profile ids.
class ProfileRegistry : public QObject
{
    Q_OBJECT

signals:
    void profileChanged(const QString &id);

public:
    // Registry over <data root>/profiles.json.
    explicit ProfileRegistry(QObject *parent = nullptr);
    // Registry over an explicit file — for tests.
    explicit ProfileRegistry(const QString &filePath,
                             QObject *parent = nullptr);

    // Lazy qApp-owned singleton (LanguageManager pattern).
    static ProfileRegistry *instance();

    static QString defaultProfileId();
    static bool isValidProfileId(const QString &id);
    // MPROF05's create flow: display name -> filesystem slug.
    static QString slugify(const QString &displayName);

    QList<ProfileContext> profiles() const;
    // Unknown or invalid id -> invalid ProfileContext.
    ProfileContext profile(const QString &id) const;
    ProfileContext activeProfile() const;
    // The ambient persona — the focused window's bound profile id
    // (MPROF03).
    QString activeProfileId() const;

    // name/avatar/createdAt are mutable through the context; the id
    // selects the entry and is itself immutable.  Unknown id -> false,
    // no write.  Persists to disk on success.
    bool updateProfile(const ProfileContext &context);

    QString filePath() const;
    // Atomic write (QSaveFile temp+rename).
    bool save();
    // Re-read from disk — for callers that changed the file behind
    // the registry's back.
    void reload();

private:
    void load();

    QString m_filePath;
    QList<ProfileContext> m_profiles;
};

#endif // PROFILEREGISTRY_H
