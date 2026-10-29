/*
 * Copyright 2026 Arktos Browser - IT-Space
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
#include "browserprofile.h"
#include "profileidentity.h"
#include "profileregistry.h"
#include "profilevault.h"

#include <qcoreapplication.h>
#include <qdatetime.h>
#include <qdir.h>
#include <qdiriterator.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qguiapplication.h>
#include <qicon.h>
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
    // sanitized.  CA08: the check lives on ProfileRegistry — the two
    // registries share the profiles.json codec.
    return ProfileRegistry::isValidProfileId(id);
}

// VSEC01's avatar-basename rule lives on ProfileRegistry —
// sanitizedAvatarName (CA08, shared with the lane's codec).

QString ProfileStore::slugify(const QString &displayName)
{
    return ProfileRegistry::slugify(displayName);
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
    // MPROF03: windows bind to registry personas now — the profile
    // manager's "Open Window" entry is honest.
    return true;
}

QString ProfileStore::activeProfileId()
{
    // MPROF03: the ambient persona — the focused window's binding.
    return BrowserPaths::activeProfileId();
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
    if (file.open(QIODevice::ReadOnly))
        m_profiles = ProfileRegistry::parseProfilesDocument(
            file.readAll());

    // The registry always describes at least the default profile.
    // Its display name seeds from the PROF01-era QSettings key when
    // the entry is first synthesized — afterwards syncIdentityName
    // keeps the two honest while this lane's identity still lives in
    // QSettings.
    bool created = false;
    ProfileRegistry::ensureDefaultEntry(&m_profiles, tr("Profile"),
                                        &created);
    // A fresh install materializes the registry on first read; a
    // corrupt file is kept on disk untouched — the next write
    // rewrites it cleanly.
    if (created && !hadFile)
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
    switch (ProfileRegistry::upsertProfile(&m_profiles, context)) {
    case ProfileRegistry::Upsert::Replaced:
        if (!save())
            return false;
        emit profileChanged(context.id);
        return true;
    case ProfileRegistry::Upsert::NoChange:
        return true;
    default:
        return false;
    }
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
        // PVAULT03/PVAULT04: deleting a profile wipes its sealed
        // vault too — the owner can always remove their own data, no
        // password needed (documented in the PVAULT04 spec).  A
        // profile whose windows are still open refuses the wipe; the
        // dir removal below proceeds either way, an orphaned vault
        // dir is ciphertext-only residue.
        ProfileVault::instance()->wipeVault(id);
        // MEM10: the persona's live objects die with it — every
        // instanceFor() map, the cookie jars and the named engine
        // profile (MPROF02's destroy-on-delete contract).  Runs
        // before the dir removal so the managers' final flushes land
        // inside the tree that is about to be removed rather than
        // recreating it.  releasePersona() refuses while a window is
        // still bound to the id — CONT01: honor it.  Removing the
        // data dir under live stores would leave the still-alive
        // managers (history, sessions, container registry, the named
        // engine profile) flushing into a deleted tree — a zombie
        // profile dir that reappears behind the wipe.  Callers that
        // promise "windows will close" must destroy the bound
        // windows first.
        if (!BrowserProfile::releasePersona(id))
            return false;
        if (QFile::exists(dir) && !QDir(dir).removeRecursively())
            return false;
        const ProfileContext removed = m_profiles.at(i);
        m_profiles.removeAt(i);
        if (!save()) {
            m_profiles.insert(i, removed);
            return false;
        }
        // The MPROF01 registry shares this file — drop its stale row
        // before anything resolves the deleted persona through it.
        ProfileRegistry::instance()->reload();
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
    // than painting nothing (ProfileIdentity's two-step rasterizer).
    if (source.isNull())
        source = ProfileIdentity::silhouetteAvatar();
    return ProfileIdentity::avatarPixmapFor(source, extent);
}

QString ProfileStore::filePath() const
{
    return m_filePath;
}

bool ProfileStore::save()
{
    QSaveFile file(m_filePath);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(ProfileRegistry::profilesDocument(m_profiles));
    return file.commit();
}

void ProfileStore::reload()
{
    load();
}
