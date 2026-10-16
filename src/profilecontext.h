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

#ifndef PROFILECONTEXT_H
#define PROFILECONTEXT_H

#include <qdatetime.h>
#include <qstring.h>

// MPROF01: one browser profile — the Firefox-profiles/Chrome-personas
// unit of isolation (own settings, history, bookmarks, cookies and
// cache under profiles/<id>/).  Value type carried by ProfileRegistry.
// The id is a filesystem slug and immutable once the profile exists;
// display name and avatar file name stay editable.
class ProfileContext
{
public:
    ProfileContext() = default;
    ProfileContext(const QString &id, const QString &name,
                   const QString &avatar = QString(),
                   const QDateTime &createdAt = QDateTime())
        : id(id)
        , name(name)
        , avatar(avatar)
        , createdAt(createdAt)
    {
    }

    bool isValid() const { return !id.isEmpty(); }

    bool operator==(const ProfileContext &other) const
    {
        return id == other.id && name == other.name
               && avatar == other.avatar && createdAt == other.createdAt;
    }
    bool operator!=(const ProfileContext &other) const
    {
        return !(*this == other);
    }

    QString id;         // slug — immutable, also the profiles/<id>/ dir name
    QString name;       // display name
    QString avatar;     // avatar file name inside the profile dir (may be empty)
    QDateTime createdAt;
};

#endif // PROFILECONTEXT_H
