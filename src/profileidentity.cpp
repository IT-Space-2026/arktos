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

#include "browserpaths.h"
#include "profilecontext.h"
#include "profileregistry.h"

#include <qapplication.h>
#include <qfile.h>
#include <qguiapplication.h>
#include <qhash.h>
#include <qimage.h>
#include <qimagereader.h>
#include <qpainter.h>
#include <qpainterpath.h>
#include <qsavefile.h>

static const char kSilhouetteResource[] =
    ":/graphics/profile-silhouette.svg";

ProfileIdentity::ProfileIdentity(const QString &profileId,
                                 QObject *parent)
    : QObject(parent)
    , m_profileId(BrowserPaths::isProfileId(profileId)
                  ? profileId : QStringLiteral("default"))
    , m_avatarLoaded(false)
{
    m_name = name();
    m_avatarFileName = avatarFileName();
    // MPROF01: the profile registry is the single source of truth for
    // name/avatar — an updateProfile() from anywhere (e.g. MPROF05's
    // manager UI) repaints through the usual signals.  MPROF03: the
    // filter keys on THIS instance's persona — a rename under another
    // profile must not repaint (or worse, cache) here.
    connect(ProfileRegistry::instance(), &ProfileRegistry::profileChanged,
            this, [this](const QString &id) {
        if (id != m_profileId)
            return;
        const QString current = name();
        if (current != m_name) {
            m_name = current;
            emit nameChanged(m_name);
        }
        const QString file = avatarFileName();
        if (file != m_avatarFileName) {
            m_avatarFileName = file;
            m_avatarLoaded = false;
            m_avatar = QPixmap();
            m_sourceLoaded = false;
            m_source = QImage();
            emit avatarChanged();
        }
    });
}

ProfileIdentity *ProfileIdentity::instance()
{
    return instanceFor(BrowserPaths::activeProfileId());
}

ProfileIdentity *ProfileIdentity::instanceFor(const QString &profileId)
{
    QString id = profileId;
    if (!BrowserPaths::isProfileId(id))
        id = ProfileRegistry::defaultProfileId();
    static QHash<QString, ProfileIdentity *> identities;
    ProfileIdentity *&identity = identities[id];
    if (!identity)
        identity = new ProfileIdentity(id, qApp);
    return identity;
}

QString ProfileIdentity::profileId() const
{
    return m_profileId;
}

QString ProfileIdentity::name() const
{
    const QString name =
        ProfileRegistry::instance()->profile(m_profileId).name;
    return name.isEmpty() ? tr("Profile") : name;
}

void ProfileIdentity::setName(const QString &name)
{
    const QString trimmed = name.trimmed();
    const QString effective = trimmed.isEmpty() ? tr("Profile") : trimmed;
    ProfileRegistry *registry = ProfileRegistry::instance();
    ProfileContext context = registry->profile(m_profileId);
    if (!context.isValid() || effective == this->name())
        return;
    context.name = effective;
    m_name = effective;
    registry->updateProfile(context);
    emit nameChanged(m_name);
}

QString ProfileIdentity::avatarFileName() const
{
    const QString file =
        ProfileRegistry::instance()->profile(m_profileId).avatar;
    return file.isEmpty() ? QStringLiteral("profile-avatar.png") : file;
}

QString ProfileIdentity::avatarFilePath() const
{
    return BrowserPaths::profileDataFilePath(m_profileId, avatarFileName());
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

QImage ProfileIdentity::avatarSource() const
{
    if (!m_sourceLoaded) {
        m_sourceLoaded = true;
        if (QFile::exists(avatarFilePath())) {
            QImageReader reader(avatarFilePath());
            // A user-dropped file can be arbitrarily large — cap the
            // decode so a giant photo cannot balloon memory.  PROF02's
            // writer produces 128px PNGs; this bound only guards
            // hand-placed files.
            const QSize size = reader.size();
            if (size.width() > 2048 || size.height() > 2048)
                reader.setScaledSize(
                    size.scaled(512, 512, Qt::KeepAspectRatio));
            m_source = reader.read();
        }
        // PROF02: an unreadable/corrupt avatar file falls back to the
        // silhouette rather than painting nothing.
        if (m_source.isNull())
            m_source = QImageReader(
                QLatin1String(kSilhouetteResource)).read();
    }
    return m_source;
}

QPixmap ProfileIdentity::avatarPixmap(int extent) const
{
    const QImage source = avatarSource();
    if (source.isNull() || extent <= 0)
        return QPixmap();
    QGuiApplication *guiApp =
        qobject_cast<QGuiApplication *>(QCoreApplication::instance());
    const qreal dpr = guiApp ? guiApp->devicePixelRatio() : 1.0;
    const int side = qMax(1, qRound(extent * dpr));
    QImage out(side, side, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath clip;
    clip.addEllipse(QRectF(0, 0, side, side));
    painter.setClipPath(clip);
    // Cover-fit: scale the source so the square is fully painted —
    // a non-square crop can letterbox, never leave transparent
    // wedges inside the disc.
    const QSize scaled = source.size().scaled(
        side, side, Qt::KeepAspectRatioByExpanding);
    painter.drawImage(
        QRect((side - scaled.width()) / 2,
              (side - scaled.height()) / 2,
              scaled.width(), scaled.height()),
        source);
    painter.end();
    QPixmap pixmap = QPixmap::fromImage(out);
    pixmap.setDevicePixelRatio(dpr);
    return pixmap;
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
    m_sourceLoaded = false;
    m_source = QImage();
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
    m_sourceLoaded = false;
    m_source = QImage();
    if (hadAvatar)
        emit avatarChanged();
    return true;
}

void ProfileIdentity::reloadAvatar()
{
    loadAvatar();
    m_sourceLoaded = false;
    m_source = QImage();
    emit avatarChanged();
}
