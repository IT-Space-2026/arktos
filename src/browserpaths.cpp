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

#include "browserpaths.h"

#include <qcoreapplication.h>
#include <qdebug.h>
#include <qdir.h>
#include <qfile.h>
#include <qmutex.h>
#include <qsavefile.h>
#include <qstandardpaths.h>

namespace BrowserPaths {

// MPROF01 profile-owned payloads: the audit of every dataFilePath()
// call site (MPROF02's per-profile singleton list is the same set)
// plus every file rustcore resolves under its single <data dir> —
// rustCoreEnsureDataDir() anchors that dir to the profile store, so
// session.dat/credentials.dat/autofill-store.dat/sitedecisions.json/
// searchengines.json live per-profile by construction and the
// cache/rule files rust reads there (tranco-list.dat,
// blocklist-domains.txt, urlstrip-rules.json) ride along.  Those
// last three are machine-global in spirit but splitting them out of
// the crate's one data dir is impossible without rust changes; they
// repopulate themselves per profile, which the task notes record as
// the accepted trade-off.
// Truly global entries — tor/ (daemon state + staged binary), locale/
// drops, crashreport.log, arora-profile-wipe.pending, QtWebEngine/
// (Chromium namespaces it by storageName itself), profiles.json,
// this marker — deliberately absent: they stay at the data root and
// their call sites use globalDataFilePath().
static const char *kProfilePayloads[] = {
    // The SQLite WAL sidecars ride with history.db — moving the db
    // without them would silently drop every un-checkpointed entry.
    "history", "history.db", "history.db-wal", "history.db-shm",
    "icons",
    "bookmarks.xbel",
    "autofill.dat", "autofill-store.dat",
    "session.dat", "credentials.dat",
    "sitedecisions.json", "searchengines.json",
    "tranco-list.dat", "blocklist-domains.txt", "urlstrip-rules.json",
    "securestore.key", "securestore.kdf",
    "containers",
    "userscripts", "extension-updates", "extstorage",
    "notes",
    "searchengines",
    "clientcertificates",
    "servo", "pdf-view", "downloads-parts",
    "profile-avatar.png",
};

// Adblock subscriptions are one file per list (sha1-suffixed plus the
// custom list), and stores retire their superseded copies as
// "<name>.migrated" snapshots — prefix/suffix globs rather than
// fixed names.
static const char *kProfileGlobs[] = {
    "adblock_subscription_*",
    "*.migrated",
};

static const char kMigrationMarker[] = "migration-v1.done";
static const char kMigrationFailedReport[] = "migration-v1.failed";

// Every failure path funnels here: qCritical for the terminal plus a
// persistent report next to the data — a GUI-launched browser cannot
// raise a dialog this early (the application object does not exist
// yet), so the split state is discoverable rather than silent.
// Nothing was deleted, only renamed, and the next boot retries.
static void reportMigrationFailure(const QString &root,
                                   const QString &message,
                                   QString *error)
{
    if (error)
        *error = message;
    qCritical() << "BrowserPaths: profile layout migration failed -"
                << message;
    QFile report(root + QLatin1Char('/')
                 + QLatin1String(kMigrationFailedReport));
    if (report.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        report.write(message.toUtf8());
        report.write("\n");
    }
}

QString dataRoot()
{
    QString directory =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (directory.isEmpty())
        directory = QDir::homePath() + QLatin1String("/.")
                    + QCoreApplication::applicationName();
    if (!QFile::exists(directory)) {
        QDir dir;
        dir.mkpath(directory);
    }
    return directory;
}

QString globalDataFilePath(const QString &fileName)
{
    return dataRoot() + QLatin1Char('/') + fileName;
}

bool isProfileId(const QString &id)
{
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

QString activeProfileId()
{
    // MPROF01 is plumbing only: every unscoped store belongs to the
    // default profile.  MPROF03 replaces this with the caller's
    // window-bound profile id.
    return QStringLiteral("default");
}

QString profileDataDir(const QString &profileId)
{
    QString id = profileId;
    if (!isProfileId(id)) {
        qWarning() << "BrowserPaths: refusing non-slug profile id"
                   << profileId << "- using default";
        id = QStringLiteral("default");
    }
    const QString directory =
        dataRoot() + QLatin1String("/profiles/") + id;
    if (!QFile::exists(directory)) {
        QDir dir;
        dir.mkpath(directory);
    }
    return directory;
}

bool ensureDataLayout(QString *error)
{
    static QMutex mutex;
    QMutexLocker locker(&mutex);

    const QString root = dataRoot();
    if (root.isEmpty()) {
        if (error)
            *error = QStringLiteral("data root unavailable");
        return false;
    }

    const QString marker = root + QLatin1Char('/')
                           + QLatin1String(kMigrationMarker);
    if (QFile::exists(marker))
        return true;

    const QString defaultDir =
        root + QLatin1String("/profiles/default");
    QDir dir;
    if (!dir.mkpath(defaultDir)) {
        reportMigrationFailure(
            root, QStringLiteral("cannot create %1").arg(defaultDir),
            error);
        return false;
    }

    // Resumable rename pass: a missing source means the payload was
    // already moved or never existed, so a crashed run simply
    // continues.  A name colliding at the destination is ambiguous —
    // stop LOUDLY rather than split state.
    QStringList names;
    for (const char *name : kProfilePayloads)
        names.append(QLatin1String(name));
    QStringList globPatterns;
    for (const char *pattern : kProfileGlobs)
        globPatterns.append(QLatin1String(pattern));
    names.append(QDir(root).entryList(
        globPatterns, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot));

    for (const QString &name : names) {
        const QString src = root + QLatin1Char('/') + name;
        if (!QFile::exists(src))
            continue;
        const QString dst = defaultDir + QLatin1Char('/') + name;
        if (QFile::exists(dst)) {
            reportMigrationFailure(
                root, QStringLiteral(
                    "%1 exists in both the root and profiles/default")
                    .arg(name),
                error);
            return false;
        }
        if (!QFile::rename(src, dst)) {
            reportMigrationFailure(
                root, QStringLiteral("rename %1 -> %2 failed")
                    .arg(src, dst),
                error);
            return false;
        }
    }

    // The marker asserts a clean pass; without it a later boot resumes
    // the move rather than trusting a half-migrated tree.
    QSaveFile markerFile(marker);
    if (!markerFile.open(QIODevice::WriteOnly)
        || markerFile.write("v1\n") == -1
        || !markerFile.commit()) {
        reportMigrationFailure(
            root, QStringLiteral("cannot write %1").arg(marker),
            error);
        return false;
    }

    // A clean pass retires the stale report from an earlier failed
    // boot — the retry healed the tree.
    QFile::remove(root + QLatin1Char('/')
                  + QLatin1String(kMigrationFailedReport));
    return true;
}

static void ensureDataLayoutOnce()
{
    static const bool done = ensureDataLayout();
    Q_UNUSED(done);
}

QString profileDataFilePath(const QString &profileId,
                            const QString &fileName)
{
    ensureDataLayoutOnce();
    return profileDataDir(profileId) + QLatin1Char('/') + fileName;
}

QString dataFilePath(const QString &fileName)
{
    ensureDataLayoutOnce();
    return profileDataDir(activeProfileId()) + QLatin1Char('/')
           + fileName;
}

} // namespace BrowserPaths
