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

#include "profileregistry.h"

#include "browserpaths.h"

#include <qapplication.h>
#include <qfile.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qsavefile.h>
#include <qsettings.h>

ProfileRegistry::ProfileRegistry(QObject *parent)
    : QObject(parent)
    , m_filePath(BrowserPaths::globalDataFilePath(
          QLatin1String("profiles.json")))
{
    load();
}

ProfileRegistry::ProfileRegistry(const QString &filePath, QObject *parent)
    : QObject(parent)
    , m_filePath(filePath)
{
    load();
}

ProfileRegistry *ProfileRegistry::instance()
{
    static ProfileRegistry *registry = nullptr;
    if (!registry)
        registry = new ProfileRegistry(qApp);
    return registry;
}

QString ProfileRegistry::defaultProfileId()
{
    return QStringLiteral("default");
}

bool ProfileRegistry::isValidProfileId(const QString &id)
{
    // Filesystem slug: lowercase letters/digits with '-' separators —
    // the id becomes a profiles/<id>/ path segment, so '.', '/', '..'
    // and friends are refused outright rather than sanitized.
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

// VSEC01: the avatar field is a FILENAME joined under
// profiles/<id>/ — a registry value carrying separators or dot
// segments would turn avatar reads, the PNG write and the remove
// into path traversal.  Only a plain basename survives.
static QString sanitizedAvatarName(const QString &name)
{
    if (name.isEmpty() || name.size() > 255
        || name.contains(QLatin1Char('/'))
        || name.contains(QLatin1Char('\\'))
        || name == QLatin1String(".")
        || name == QLatin1String(".."))
        return QString();
    return name;
}

QString ProfileRegistry::slugify(const QString &displayName)
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

void ProfileRegistry::load()
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
            context.avatar = sanitizedAvatarName(
                entry.value(QLatin1String("avatar")).toString());
            context.createdAt = QDateTime::fromString(
                entry.value(QLatin1String("createdAt")).toString(),
                Qt::ISODate);
            if (isValidProfileId(context.id))
                m_profiles.append(context);
        }
    }

    // The registry always describes at least the default profile.
    // Seed its display name from the PROF02-era QSettings key when the
    // entry is first synthesized — that import is one-shot; the key
    // stays readable-only afterwards (MPROF04 owns QSettings).
    if (profile(defaultProfileId()).isValid())
        return;
    ProfileContext fallback;
    fallback.id = defaultProfileId();
    fallback.name = QSettings()
        .value(QLatin1String("profile/name")).toString();
    if (fallback.name.isEmpty())
        fallback.name = tr("Profile");
    fallback.avatar = QStringLiteral("profile-avatar.png");
    fallback.createdAt = QDateTime::currentDateTime();
    m_profiles.append(fallback);
    // A fresh install materializes the registry on first read; a
    // corrupt file is kept on disk untouched — the next updateProfile
    // rewrites it cleanly.
    if (!hadFile)
        save();
}

QList<ProfileContext> ProfileRegistry::profiles() const
{
    return m_profiles;
}

ProfileContext ProfileRegistry::profile(const QString &id) const
{
    for (const ProfileContext &context : m_profiles) {
        if (context.id == id)
            return context;
    }
    return ProfileContext();
}

QString ProfileRegistry::activeProfileId() const
{
    // MPROF03: the ambient persona follows the focused window — the
    // window pushes its bound id via BrowserPaths at construction and
    // on activation, so registry consumers (ProfileIdentity, profile
    // pickers) read the right persona without threading a window
    // pointer through every call.
    return BrowserPaths::activeProfileId();
}

ProfileContext ProfileRegistry::activeProfile() const
{
    return profile(activeProfileId());
}

bool ProfileRegistry::updateProfile(const ProfileContext &context)
{
    if (!isValidProfileId(context.id))
        return false;
    // A traversal-shaped avatar must never reach the file even when a
    // caller hands updateProfile one directly.
    ProfileContext clean = context;
    clean.avatar = sanitizedAvatarName(clean.avatar);
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (m_profiles.at(i).id == clean.id) {
            if (m_profiles.at(i) == clean)
                return true;
            m_profiles[i] = clean;
            if (!save())
                return false;
            emit profileChanged(context.id);
            return true;
        }
    }
    return false;
}

QString ProfileRegistry::filePath() const
{
    return m_filePath;
}

bool ProfileRegistry::save()
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

void ProfileRegistry::reload()
{
    load();
}
