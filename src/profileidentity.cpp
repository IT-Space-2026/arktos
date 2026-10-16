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

#include <qcoreapplication.h>
#include <qfile.h>
#include <qguiapplication.h>
#include <qimagereader.h>
#include <qpainter.h>
#include <qpainterpath.h>
#include <qsettings.h>

static const char kNameKey[] = "profile/name";
static const char kAvatarFileName[] = "profile-avatar.png";
static const char kSilhouetteResource[] =
    ":/graphics/profile-silhouette.svg";

ProfileIdentity::ProfileIdentity(QObject *parent)
    : QObject(parent)
{
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
    return QSettings()
        .value(QLatin1String(kNameKey), tr("Profile"))
        .toString();
}

void ProfileIdentity::setName(const QString &name)
{
    const QString trimmed = name.trimmed();
    const QString effective = trimmed.isEmpty() ? tr("Profile") : trimmed;
    if (effective == this->name())
        return;
    QSettings().setValue(QLatin1String(kNameKey), effective);
    emit nameChanged(effective);
}

QString ProfileIdentity::avatarFilePath() const
{
    return BrowserPaths::dataFilePath(QLatin1String(kAvatarFileName));
}

bool ProfileIdentity::hasCustomAvatar() const
{
    return QFile::exists(avatarFilePath());
}

QImage ProfileIdentity::avatarSource() const
{
    if (!m_sourceLoaded) {
        m_sourceLoaded = true;
        if (hasCustomAvatar()) {
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

void ProfileIdentity::reloadAvatar()
{
    m_sourceLoaded = false;
    m_source = QImage();
    emit avatarChanged();
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
