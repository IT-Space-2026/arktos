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

#ifndef PROFILEIDENTITY_H
#define PROFILEIDENTITY_H

#include <qobject.h>

#include <qpixmap.h>
#include <qstring.h>

class QImage;

// PROF02: qApp-owned profile identity — display name + custom avatar.
// MPROF01: both delegate to the active profile's ProfileRegistry
// entry — the name lives in profiles.json and the avatar file under
// the profile's own dir (profiles/<id>/).  When the file is absent
// or undecodable avatar() returns a null pixmap and callers render
// the bundled silhouette instead (that graphic is PROF01's — this
// class only owns the custom-avatar pipeline).  The avatar file is
// user data: single-category ClearPrivateData clears must NOT remove
// it; only the PROF03 full-profile wipe does.
//
// PROF01: avatarPixmap(extent) returns the identity's face the way the
// nav-bar profile button and menu header render it — circular-cropped
// and cover-scaled, custom avatar when present else the bundled
// silhouette graphic.
class ProfileIdentity : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString name READ name WRITE setName NOTIFY nameChanged)

signals:
    void nameChanged(const QString &name);
    void avatarChanged();

public:
    // MPROF03: identity is persona-bound — each profile id gets its
    // own instance whose name/avatar reads its registry row and whose
    // caches and change-filtered signals stay inside the persona.
    explicit ProfileIdentity(const QString &profileId,
                             QObject *parent = nullptr);

    // Lazy qApp-owned singletons keyed by persona — instance() is the
    // ambient (focused window's) persona for context-less callers.
    static ProfileIdentity *instance();
    static ProfileIdentity *instanceFor(const QString &profileId);

    // The persona this identity is bound to.
    QString profileId() const;

    // On-disk avatar edge length written by setAvatarImage().
    static constexpr int AvatarSize = 128;

    QString name() const;
    void setName(const QString &name);

    // Null when no usable custom avatar exists — callers then render
    // the default silhouette.
    QPixmap avatar();
    bool hasCustomAvatar();

    QString avatarFilePath() const;

    // Circular-cropped, cover-scaled avatar at `extent` logical
    // pixels — the custom file when present, else the bundled
    // silhouette.  A null pixmap means even the fallback failed.
    QPixmap avatarPixmap(int extent) const;

    // Scales image to AvatarSize and writes the avatar PNG atomically
    // (QSaveFile's temp+rename).  Returns false on a null image or a
    // write failure; emits avatarChanged() on success.
    bool setAvatarImage(const QImage &image);
    // Deletes the avatar file -> silhouette fallback.  Returns false
    // only when an existing file could not be removed.
    bool removeAvatar();
    // Re-reads the avatar file — for callers that changed it behind
    // the singleton's back.
    void reloadAvatar();

private:
    bool loadAvatar();
    QImage avatarSource() const;
    QString avatarFileName() const;

    QString m_profileId;
    QString m_name;
    QString m_avatarFileName;
    QPixmap m_avatar;
    bool m_avatarLoaded;
    mutable bool m_sourceLoaded = false;
    mutable QImage m_source;
};

#endif // PROFILEIDENTITY_H
