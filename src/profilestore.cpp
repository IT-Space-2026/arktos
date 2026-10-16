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

#include "profilestore.h"

#include "browserpaths.h"
#include "profileidentity.h"

#include <qcoreapplication.h>
#include <qdatetime.h>
#include <qdir.h>
#include <qdiriterator.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qguiapplication.h>
#include <qimagereader.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qpainter.h>
#include <qpainterpath.h>
#include <qsavefile.h>
#include <qsettings.h>
#include <qstandardpaths.h>

static const char kAvatarFileName[] = "profile-avatar.png";
static const char kSilhouetteResource[] =
    ":/graphics/profile-silhouette.svg";

// The registry file sits next to the profiles/ dir it describes, so a
// store opened on an explicit path (tests) roots its whole tree there.
static QString rootForFile(const QString &filePath)
{
    return QFileInfo(filePath).absolutePath();
}

ProfileStore::ProfileStore(QObject *parent)
    : QObject(parent)
    , m_filePath(dataRoot() + QLatin1String("/profiles.json"))
{
    load();
    connect(ProfileIdentity::instance(), &ProfileIdentity::nameChanged,
            this, [this](const QString &name) { syncIdentityName(name); });
}

ProfileStore::ProfileStore(const QString &filePath, QObject *parent)
    : QObject(parent)
    , m_filePath(filePath)
{
    load();
    connect(ProfileIdentity::instance(), &ProfileIdentity::nameChanged,
            this, [this](const QString &name) { syncIdentityName(name); });
}

ProfileStore *ProfileStore::instance()
{
    static ProfileStore *store = nullptr;
    if (!store)
        store = new ProfileStore(qApp);
    return store;
}

QString ProfileStore::defaultProfileId()
{
    return QStringLiteral("default");
}

bool ProfileStore::isValidProfileId(const QString &id)
{
    // Filesystem slug: lowercase letters/digits with '-'/'_'
    // separators — the id becomes a profiles/<id>/ path segment, so
    // '.', '/', '..' and friends are refused outright rather than
    // sanitized.
    if (id.isEmpty() || id.size() > 64)
        return false;
    for (const QChar c : id) {
        const ushort u = c.unicode();
        const bool ok = (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9')
                        || c == QLatin1Char('-') || c == QLatin1Char('_');
        if (!ok)
            return false;
    }
    return true;
}

QString ProfileStore::slugify(const QString &displayName)
{
    QString slug;
    bool lastWasDash = false;
    for (const QChar c : displayName.toLower()) {
        if ((c >= QLatin1Char('a') && c <= QLatin1Char('z'))
            || (c >= QLatin1Char('0') && c <= QLatin1Char('9'))) {
            slug.append(c);
            lastWasDash = false;
        } else if (!slug.isEmpty() && !lastWasDash) {
            slug.append(QLatin1Char('-'));
            lastWasDash = true;
        }
    }
    while (slug.endsWith(QLatin1Char('-')))
        slug.chop(1);
    if (slug.isEmpty())
        slug = QStringLiteral("profile");
    return slug.left(64);
}

QString ProfileStore::dataRoot()
{
    QString directory = QStandardPaths::writableLocation(
        QStandardPaths::AppDataLocation);
    if (directory.isEmpty())
        directory = QDir::homePath() + QLatin1String("/.")
                    + QCoreApplication::applicationName();
    if (!QFile::exists(directory)) {
        QDir dir;
        dir.mkpath(directory);
    }
    return directory;
}

QString ProfileStore::profileDir(const QString &id) const
{
    QString effective = id;
    // A bad id can never escape the profiles/ tree — same fallback
    // MPROF01's profileDataDir applies.
    if (!isValidProfileId(effective))
        effective = defaultProfileId();
    return rootForFile(m_filePath) + QLatin1String("/profiles/")
           + effective;
}

QString ProfileStore::profileFilePath(const QString &id,
                                      const QString &fileName) const
{
    return profileDir(id) + QLatin1Char('/') + fileName;
}

bool ProfileStore::windowBindingAvailable()
{
    // MPROF03 (window<->profile binding) has not merged into this
    // lane — every window runs the default profile, so a per-profile
    // "open window here" is disabled rather than faked.
    return false;
}

QString ProfileStore::activeProfileId()
{
    return defaultProfileId();
}

ProfileContext ProfileStore::activeProfile() const
{
    return profile(activeProfileId());
}

void ProfileStore::load()
{
    m_profiles.clear();
    const bool hadFile = QFile::exists(m_filePath);
    QFile file(m_filePath);
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonObject root = QJsonDocument::fromJson(file.readAll())
            .object();
        const QJsonArray entries = root.value(QLatin1String("profiles"))
            .toArray();
        for (const QJsonValue &value : entries) {
            const QJsonObject entry = value.toObject();
            ProfileContext context;
            context.id = entry.value(QLatin1String("id")).toString();
            context.name = entry.value(QLatin1String("name")).toString();
            context.avatar =
                entry.value(QLatin1String("avatar")).toString();
            context.createdAt = QDateTime::fromString(
                entry.value(QLatin1String("createdAt")).toString(),
                Qt::ISODate);
            if (isValidProfileId(context.id))
                m_profiles.append(context);
        }
    }

    // The registry always describes at least the default profile.
    // Its display name seeds from the PROF01-era QSettings key when
    // the entry is first synthesized — afterwards syncIdentityName
    // keeps the two honest while this lane's identity still lives in
    // QSettings.
    if (profile(defaultProfileId()).isValid())
        return;
    ProfileContext fallback;
    fallback.id = defaultProfileId();
    fallback.name = QSettings()
        .value(QLatin1String("profile/name")).toString();
    if (fallback.name.isEmpty())
        fallback.name = tr("Profile");
    fallback.avatar = QLatin1String(kAvatarFileName);
    fallback.createdAt = QDateTime::currentDateTime();
    m_profiles.append(fallback);
    // A fresh install materializes the registry on first read; a
    // corrupt file is kept on disk untouched — the next write
    // rewrites it cleanly.
    if (!hadFile)
        save();
}

QList<ProfileContext> ProfileStore::profiles() const
{
    return m_profiles;
}

ProfileContext ProfileStore::profile(const QString &id) const
{
    for (const ProfileContext &context : m_profiles) {
        if (context.id == id)
            return context;
    }
    return ProfileContext();
}

bool ProfileStore::updateProfile(const ProfileContext &context)
{
    if (!isValidProfileId(context.id))
        return false;
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (m_profiles.at(i).id == context.id) {
            if (m_profiles.at(i) == context)
                return true;
            m_profiles[i] = context;
            if (!save())
                return false;
            emit profileChanged(context.id);
            return true;
        }
    }
    return false;
}

ProfileContext ProfileStore::createProfile(const QString &displayName)
{
    const QString name = displayName.trimmed();
    if (name.isEmpty())
        return ProfileContext();

    const QString base = slugify(name);
    QString id = base;
    for (int n = 2; profile(id).isValid()
                    || QFile::exists(profileDir(id)); ++n)
        id = base + QLatin1Char('-') + QString::number(n);

    ProfileContext context;
    context.id = id;
    context.name = name;
    context.createdAt = QDateTime::currentDateTime();
    m_profiles.append(context);
    if (!QDir().mkpath(profileDir(id)) || !save()) {
        m_profiles.removeLast();
        return ProfileContext();
    }
    emit profilesChanged();
    return context;
}

bool ProfileStore::renameProfile(const QString &id, const QString &name)
{
    ProfileContext context = profile(id);
    if (!context.isValid())
        return false;
    const QString trimmed = name.trimmed();
    context.name = trimmed.isEmpty() ? tr("Profile") : trimmed;
    if (!updateProfile(context))
        return false;
    if (id == activeProfileId())
        ProfileIdentity::instance()->setName(context.name);
    return true;
}

void ProfileStore::syncIdentityName(const QString &name)
{
    // The PROF01 rename path (nav-button menu) writes QSettings only —
    // mirror it into the default row so the manager shows one truth.
    ProfileContext context = profile(defaultProfileId());
    if (context.isValid() && context.name != name) {
        context.name = name;
        updateProfile(context);
    }
}

bool ProfileStore::writeAvatarFile(const QString &id,
                                   const QString &sourcePath)
{
    QImageReader reader(sourcePath);
    // A user-dropped file can be arbitrarily large — cap the decode
    // the way ProfileIdentity's reader does.
    const QSize size = reader.size();
    if (size.width() > 2048 || size.height() > 2048)
        reader.setScaledSize(size.scaled(512, 512, Qt::KeepAspectRatio));
    QImage image = reader.read();
    if (image.isNull())
        return false;
    if (image.width() > 128 || image.height() > 128)
        image = image.scaled(128, 128, Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
    if (!QDir().mkpath(profileDir(id)))
        return false;
    return image.save(profileFilePath(id, QLatin1String(kAvatarFileName)),
                      "PNG");
}

bool ProfileStore::setAvatarFile(const QString &id,
                                 const QString &sourcePath)
{
    ProfileContext context = profile(id);
    if (!context.isValid())
        return false;
    if (!writeAvatarFile(id, sourcePath))
        return false;
    context.avatar = QLatin1String(kAvatarFileName);
    if (!updateProfile(context))
        return false;
    if (id == activeProfileId()) {
        // The live identity still reads the legacy data-root file —
        // mirror the bytes there and repaint every window's button.
        QFile::remove(BrowserPaths::dataFilePath(
            QLatin1String(kAvatarFileName)));
        QFile::copy(profileFilePath(id, QLatin1String(kAvatarFileName)),
                    BrowserPaths::dataFilePath(
                        QLatin1String(kAvatarFileName)));
        ProfileIdentity::instance()->reloadAvatar();
    }
    return true;
}

bool ProfileStore::removeAvatar(const QString &id)
{
    ProfileContext context = profile(id);
    if (!context.isValid())
        return false;
    const QString fileName = context.avatar.isEmpty()
        ? QLatin1String(kAvatarFileName) : context.avatar;
    QFile::remove(profileFilePath(id, fileName));
    context.avatar = QString();
    if (!updateProfile(context))
        return false;
    if (id == activeProfileId()) {
        QFile::remove(BrowserPaths::dataFilePath(
            QLatin1String(kAvatarFileName)));
        ProfileIdentity::instance()->reloadAvatar();
    }
    return true;
}

bool ProfileStore::deleteProfile(const QString &id)
{
    // The default profile is the browser's own data — removing it is
    // the full wipe, which stays PROF03's marker path's job.
    if (!isValidProfileId(id) || id == defaultProfileId())
        return false;
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (m_profiles.at(i).id != id)
            continue;
        const QString dir = profileDir(id);
        if (QFile::exists(dir) && !QDir(dir).removeRecursively())
            return false;
        const ProfileContext removed = m_profiles.at(i);
        m_profiles.removeAt(i);
        if (!save()) {
            m_profiles.insert(i, removed);
            return false;
        }
        emit profilesChanged();
        return true;
    }
    return false;
}

qint64 ProfileStore::profileDataSize(const QString &id) const
{
    const QString dir = profileDir(id);
    if (!QFile::exists(dir))
        return -1;
    qint64 bytes = 0;
    QDirIterator it(dir, QDir::Files | QDir::NoDotAndDotDot
                    | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        bytes += it.fileInfo().size();
    }
    return bytes;
}

QPixmap ProfileStore::avatarPixmap(const ProfileContext &context,
                                   int extent) const
{
    QImage source;
    if (context.isValid()) {
        const QString fileName = context.avatar.isEmpty()
            ? QLatin1String(kAvatarFileName) : context.avatar;
        const QString path = profileFilePath(context.id, fileName);
        if (QFile::exists(path)) {
            QImageReader reader(path);
            const QSize size = reader.size();
            if (size.width() > 2048 || size.height() > 2048)
                reader.setScaledSize(
                    size.scaled(512, 512, Qt::KeepAspectRatio));
            source = reader.read();
        }
    }
    // An unreadable/corrupt avatar falls back to the silhouette rather
    // than painting nothing.
    if (source.isNull())
        source = QImageReader(QLatin1String(kSilhouetteResource)).read();
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

QString ProfileStore::filePath() const
{
    return m_filePath;
}

bool ProfileStore::save()
{
    QJsonArray entries;
    for (const ProfileContext &context : m_profiles) {
        QJsonObject entry;
        entry.insert(QLatin1String("id"), context.id);
        entry.insert(QLatin1String("name"), context.name);
        entry.insert(QLatin1String("avatar"), context.avatar);
        entry.insert(QLatin1String("createdAt"),
                     context.createdAt.toString(Qt::ISODate));
        entries.append(entry);
    }
    QJsonObject root;
    root.insert(QLatin1String("version"), 1);
    root.insert(QLatin1String("profiles"), entries);

    QSaveFile file(m_filePath);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return file.commit();
}

void ProfileStore::reload()
{
    load();
}
