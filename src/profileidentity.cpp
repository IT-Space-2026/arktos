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

#include "profileidentity.h"
#include "profilesettings.h"

#include "browserpaths.h"

#include <qapplication.h>
#include <qfile.h>
#include <qimage.h>
#include <qimagereader.h>
#include <qsavefile.h>
#include <qsettings.h>

ProfileIdentity::ProfileIdentity(QObject *parent)
    : QObject(parent)
    , m_avatarLoaded(false)
{
    m_name = ProfileSettings()
        .value(QLatin1String("profile/name"), tr("Profile"))
        .toString();
}

ProfileIdentity *ProfileIdentity::instance()
{
    static ProfileIdentity *identity = nullptr;
    if (!identity)
        identity = new ProfileIdentity(qApp);
    return identity;
}

QString ProfileIdentity::name() const
{
    return m_name;
}

void ProfileIdentity::setName(const QString &name)
{
    const QString trimmed = name.trimmed();
    const QString effective = trimmed.isEmpty() ? tr("Profile") : trimmed;
    if (effective == m_name)
        return;
    m_name = effective;
    ProfileSettings().setValue(QLatin1String("profile/name"), m_name);
    emit nameChanged(m_name);
}

QString ProfileIdentity::avatarFilePath() const
{
    return BrowserPaths::dataFilePath(QLatin1String("profile-avatar.png"));
}

QPixmap ProfileIdentity::avatar()
{
    if (!m_avatarLoaded)
        loadAvatar();
    return m_avatar;
}

bool ProfileIdentity::hasCustomAvatar()
{
    return !avatar().isNull();
}

bool ProfileIdentity::loadAvatar()
{
    m_avatarLoaded = true;
    m_avatar = QPixmap();
    const QString path = avatarFilePath();
    if (!QFile::exists(path))
        return false;
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QImage image = reader.read();
    if (image.isNull())
        return false;
    m_avatar = QPixmap::fromImage(image);
    return true;
}

bool ProfileIdentity::setAvatarImage(const QImage &image)
{
    if (image.isNull())
        return false;
    const QImage scaled = image.scaled(AvatarSize, AvatarSize,
                                       Qt::IgnoreAspectRatio,
                                       Qt::SmoothTransformation);
    QSaveFile file(avatarFilePath());
    if (!file.open(QIODevice::WriteOnly))
        return false;
    if (!scaled.save(&file, "PNG"))
        return false;
    if (!file.commit())
        return false;
    m_avatar = QPixmap::fromImage(scaled);
    m_avatarLoaded = true;
    emit avatarChanged();
    return true;
}

bool ProfileIdentity::removeAvatar()
{
    const QString path = avatarFilePath();
    const bool hadAvatar = QFile::exists(path)
                           || (m_avatarLoaded && !m_avatar.isNull());
    if (QFile::exists(path) && !QFile::remove(path))
        return false;
    m_avatar = QPixmap();
    m_avatarLoaded = true;
    if (hadAvatar)
        emit avatarChanged();
    return true;
}

void ProfileIdentity::reloadAvatar()
{
    loadAvatar();
    emit avatarChanged();
}
