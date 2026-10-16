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

#ifndef PROFILESTORE_H
#define PROFILESTORE_H

#include <profilecontext.h>

#include <qobject.h>
#include <qpixmap.h>
#include <qstring.h>

// MPROF05 standalone fallback: the lane-local profile registry behind
// the Profile Manager dialog and the profile menu's switcher.  The
// main lane's MPROF01 ProfileRegistry has not merged into this branch
// yet, so this store implements the same persisted contract directly:
//
//   - <data root>/profiles.json — a flat records array under
//     "profiles" ({id, name, avatar, createdAt}, "version": 1),
//     byte-compatible with MPROF01's writer so the real registry reads
//     this file untouched when it lands.
//   - <data root>/profiles/<id>/ — a profile's private store; the id
//     is a filesystem slug (isValidProfileId), immutable once created.
//   - The registry always describes at least "default"; its display
//     name seeds from the PROF01-era QSettings "profile/name" key and
//     stays live-synced with ProfileIdentity.
//
// Seam helpers after the MPROF03 merge:
//   - windowBindingAvailable() is true — windows bind registry
//     personas, so per-profile "Open Window" entries are honest.
//   - activeProfileId() returns the ambient persona — the focused
//     window's bound id (BrowserPaths::activeProfileId).
//   - deleteProfile() removes the registry row and the profile dir
//     in place; PROF03's marker-and-restart wipe path covers the
//     live-profile case (a running window's persona is never deleted
//     out from under it).
//   - For the default profile, name/avatar writes are mirrored into
//     the PROF01-era locations (QSettings "profile/name" and the
//     data-root profile-avatar.png) so the existing ProfileIdentity
//     consumers repaint without a rework.
class ProfileStore : public QObject
{
    Q_OBJECT

signals:
    // One entry's mutable fields changed (name/avatar).
    void profileChanged(const QString &id);
    // The profile list itself changed (create/delete).
    void profilesChanged();

public:
    // Registry over <data root>/profiles.json.
    explicit ProfileStore(QObject *parent = nullptr);
    // Registry over an explicit file — the profiles/ dir roots at the
    // file's directory; for tests.
    explicit ProfileStore(const QString &filePath, QObject *parent = nullptr);

    // Lazy qApp-owned singleton (LanguageManager pattern).
    static ProfileStore *instance();

    static QString defaultProfileId();
    // Slug check: lowercase letters, digits, '-'/'_' — an id is a
    // profiles/<id>/ path segment, so anything else is refused.
    static bool isValidProfileId(const QString &id);
    // Display name -> filesystem slug ("Profile" when nothing survives).
    static QString slugify(const QString &displayName);

    // The registry file lives at its root — <data root>/profiles.json
    // for the default store, <dir>/profiles.json for explicit-path
    // stores, so test instances own a whole isolated tree.
    static QString dataRoot();
    QString profileDir(const QString &id) const;
    QString profileFilePath(const QString &id, const QString &fileName) const;

    // MPROF03: window↔profile binding is live — every window carries
    // its bound persona, so this is true and per-profile window
    // entries stay enabled.
    static bool windowBindingAvailable();
    static QString activeProfileId();
    ProfileContext activeProfile() const;

    QList<ProfileContext> profiles() const;
    // Unknown or invalid id -> invalid ProfileContext.
    ProfileContext profile(const QString &id) const;

    // Creates the registry row and profiles/<id>/ (slugified unique id
    // derived from the display name).  Returns the new context; an
    // invalid context means the name produced nothing or the write
    // failed.
    ProfileContext createProfile(const QString &displayName);

    // Display-name update; persists.  Renaming the active (default)
    // profile routes through ProfileIdentity so every window's profile
    // button and menu header repaint — the QSettings key stays the
    // identity's read path on this lane.
    bool renameProfile(const QString &id, const QString &name);

    // Copies `sourcePath` into profiles/<id>/profile-avatar.png (scaled
    // to the PROF02-era 128px square) and points the registry entry at
    // it.  For the default profile the legacy data-root copy is also
    // refreshed and ProfileIdentity reloaded, so the nav button swaps
    // faces live.  Unknown id/unreadable image -> false, no write.
    bool setAvatarFile(const QString &id, const QString &sourcePath);
    bool removeAvatar(const QString &id);

    // Removes the registry row and deletes profiles/<id>/ recursively.
    // The default profile is refused outright — deleting it is the
    // full browser-data wipe, which is PROF03's marker path and stays
    // its sole entry point.  Returns false for unknown/invalid ids.
    bool deleteProfile(const QString &id);

    // Recursive byte size of profiles/<id>/, -1 when the dir is gone.
    qint64 profileDataSize(const QString &id) const;

    // Circular-cropped, cover-scaled avatar at `extent` logical pixels
    // — the profile's avatar file when present and readable, else the
    // bundled silhouette.  Same paint contract as
    // ProfileIdentity::avatarPixmap, keyed by profile instead of the
    // single global identity.
    QPixmap avatarPixmap(const ProfileContext &context, int extent) const;

    QString filePath() const;
    // Atomic write (QSaveFile temp+rename).
    bool save();
    // Re-read from disk — for callers that changed the file behind
    // the store's back.
    void reload();

private:
    void load();
    bool updateProfile(const ProfileContext &context);
    bool writeAvatarFile(const QString &id, const QString &sourcePath);
    void syncIdentityName(const QString &name);

    QString m_filePath;
    QList<ProfileContext> m_profiles;
};

#endif // PROFILESTORE_H
