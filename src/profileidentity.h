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

#include <qimage.h>
#include <qobject.h>
#include <qpixmap.h>
#include <qstring.h>

// PROF01: the app-global profile identity behind the nav-bar profile
// button — a single-profile identity (no switcher) shared by every
// window: normal, private and tor chrome all show the same name and
// avatar.
//
//   - The display name persists in QSettings "profile/name" and
//     defaults to "Profile".  setName() writes through and emits
//     nameChanged so every open window's button + menu header
//     repaints live.
//   - The avatar is the user image at avatarFilePath() when one
//     exists (PROF02 owns writing it), otherwise the bundled
//     neutral silhouette graphic.  avatarPixmap(extent) returns the
//     source circular-cropped and cover-scaled — the shape the
//     button face and menu header both render.
//
// Lazy qApp-owned singleton (LanguageManager/HistoryManager pattern).
class ProfileIdentity : public QObject
{
    Q_OBJECT

public:
    static ProfileIdentity *instance();

    QString name() const;
    void setName(const QString &name);

    // The custom-avatar path — the file may not exist yet (PROF02
    // writes it); hasCustomAvatar() distinguishes the two faces.
    QString avatarFilePath() const;
    bool hasCustomAvatar() const;

    // Circular-cropped, cover-scaled avatar at `extent` logical
    // pixels — the custom file when present, else the bundled
    // silhouette.  A null pixmap means even the fallback failed.
    QPixmap avatarPixmap(int extent) const;

    // Drops the cached source and re-reads the avatar file — the
    // call PROF02 makes after saving/removing the file so every
    // window's button repaints.
    void reloadAvatar();

signals:
    void nameChanged(const QString &name);
    void avatarChanged();

private:
    explicit ProfileIdentity(QObject *parent = nullptr);

    QImage avatarSource() const;

    mutable bool m_sourceLoaded = false;
    mutable QImage m_source;
};

#endif // PROFILEIDENTITY_H
