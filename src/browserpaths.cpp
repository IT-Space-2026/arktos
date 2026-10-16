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
#include <qfileinfo.h>
#include <qmutex.h>
#include <qsavefile.h>
#include <qset.h>
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
// drops, crashreport.log, arora-profile-wipe.pending, profiles.json,
// this marker — deliberately absent: they stay at the data root and
// their call sites use globalDataFilePath().  QtWebEngine/ joined
// the sealed side in VSEC02 — ensureEngineStorageLayout() owns its
// relocation.
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
static const char kEngineMigrationFailedReport[] = "migration-v2.failed";

// Every failure path funnels here: qCritical for the terminal plus a
// persistent report next to the data — a GUI-launched browser cannot
// raise a dialog this early (the application object does not exist
// yet), so the split state is discoverable rather than silent.
// Nothing was deleted, only renamed, and the next boot retries.
static void reportMigrationFailure(const QString &root,
                                   const QString &message,
                                   QString *error,
                                   const char *reportName
                                       = kMigrationFailedReport)
{
    if (error)
        *error = message;
    qCritical() << "BrowserPaths: profile layout migration failed -"
                << message;
    QFile report(root + QLatin1Char('/')
                 + QLatin1String(reportName));
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

// MPROF03: the ambient persona the process's unscoped stores belong
// to.  BrowserMainWindow pushes its bound id at construction (so the
// widgets it builds bind the right persona) and on every activation
// (so focus switching keeps ambient resolution honest); when the
// focused window dies the destructor re-anchors to whatever window
// holds focus next.  Between pushes this is simply the last active
// window's persona — an empty value only exists before the first
// window is constructed, so it aliases "default".
static QString s_activeProfileId;

QString activeProfileId()
{
    if (s_activeProfileId.isEmpty())
        return QStringLiteral("default");
    return s_activeProfileId;
}

void setActiveProfileId(const QString &profileId)
{
    if (!isProfileId(profileId)) {
        if (!profileId.isEmpty())
            qWarning() << "BrowserPaths: refusing non-slug active"
                          "profile id" << profileId << "- using default";
        s_activeProfileId = QStringLiteral("default");
        return;
    }
    s_activeProfileId = profileId;
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
    // VSEC01: a symlinked profile dir is never legitimate — every
    // store would follow it outside the profiles/ tree.  The path
    // contract can't fail here (every caller needs somewhere to
    // resolve), so the honest treatment is a loud once-per-path
    // warning rather than a silent follow.
    static QSet<QString> symlinkWarned;
    if (!symlinkWarned.contains(directory)
        && QFileInfo(directory).isSymLink()) {
        symlinkWarned.insert(directory);
        qWarning() << "BrowserPaths: profile dir is a symlink and its"
                      " stores will follow it outside the profiles"
                      " tree:" << directory;
    }
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
    // VSEC01: the destination must be a real directory too — a
    // planted symlink at profiles/ or profiles/default would route
    // every rename (and mkpath's descent) to the attacker's target.
    if (QFileInfo(root + QLatin1String("/profiles")).isSymLink()
        || QFileInfo(defaultDir).isSymLink()) {
        reportMigrationFailure(
            root, QStringLiteral(
                "refusing to migrate into a symlinked profiles tree"),
            error);
        return false;
    }
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
        // VSEC01: a symlinked payload is never legitimate — nothing in
        // Arora creates links at these names.  QFile::exists() follows
        // the link and QFile::rename() would carry the LINK intact
        // into profiles/default, where every later open() resolves to
        // the attacker's target.  Fail the pass loudly instead:
        // nothing moves, nothing follows the link, and the report
        // names the artifact so the user can remove it.  isSymLink()
        // is lstat-based, so a dangling link is caught too.
        if (QFileInfo(src).isSymLink()) {
            reportMigrationFailure(
                root, QStringLiteral(
                    "refusing to migrate symlinked payload %1").arg(src),
                error);
            return false;
        }
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

QString engineStorageDir(const QString &profileId)
{
    return profileDataDir(profileId) + QLatin1String("/QtWebEngine");
}

QString engineCacheDir(const QString &profileId)
{
    return profileDataDir(profileId) + QLatin1String("/cache/QtWebEngine");
}

// A sealed blob owning the id's plaintext decides whether an engine
// tree can go straight into profiles/<id> (empty while locked —
// moving it in would break unseal's empty-destination contract) or
// parks in <id>.vault/engine-hold/ for unseal() to restore.  Both
// vault locations are checked — pre-MPROF artifacts sit as siblings
// of the data root, post-MPROF ones under profiles/.
static QString engineVaultHold(const QString &root, const QString &id)
{
    const QStringList vaultDirs = {
        root + QLatin1String("/profiles/") + id
            + QLatin1String(".vault"),
        root + QLatin1Char('/') + id + QLatin1String(".vault"),
    };
    for (const QString &vault : vaultDirs) {
        if (QFile::exists(vault + QLatin1String("/vault.arv")))
            return vault + QLatin1String("/engine-hold/QtWebEngine");
    }
    return QString();
}

// The lstat-not-follow rule shared by both layout migrations: every
// path component a rename writes through is checked for links, so a
// planted junction can never route the move outside the tree.  Only
// components strictly BELOW the data root are considered — the root
// itself may legitimately sit behind a user symlinked XDG dir.
static bool pathChainHasLink(const QString &path, const QString &root)
{
    QString current = path;
    while (current.startsWith(root + QLatin1Char('/'))
           && current != root) {
        if (QFileInfo(current).isSymLink())
            return true;
        const QString parent = QFileInfo(current).absolutePath();
        if (parent == current || parent.isEmpty())
            break;
        current = parent;
    }
    return false;
}

bool ensureEngineStorageLayout(QString *error)
{
    static QMutex mutex;
    QMutexLocker locker(&mutex);

    const QString root = dataRoot();
    if (root.isEmpty()) {
        if (error)
            *error = QStringLiteral("data root unavailable");
        return false;
    }

    const QString engineRoot = root + QLatin1String("/QtWebEngine");
    if (QFileInfo(engineRoot).isSymLink()) {
        reportMigrationFailure(
            root, QStringLiteral(
                "refusing to sweep a symlinked QtWebEngine dir"),
            error, kEngineMigrationFailedReport);
        return false;
    }

    // Dirs|Files so a DANGLING symlink named like a store still shows
    // up — QDir::Dirs alone would filter it out and never run the
    // isSymLink refusal.
    const QFileInfoList entries = QFileInfo(engineRoot).isDir()
        ? QDir(engineRoot).entryInfoList(
              QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot
              | QDir::Hidden | QDir::System)
        : QFileInfoList();
    for (const QFileInfo &entry : entries) {
        const QString name = entry.fileName();
        QString id;
        if (name == QLatin1String("arora")) {
            id = QStringLiteral("default");
        } else if (name.startsWith(QLatin1String("arora-"))
                   && !name.startsWith(
                       QLatin1String("arora-container-"))) {
            const QString slug = name.mid(6);
            if (isProfileId(slug))
                id = slug;
        }
        if (id.isEmpty())
            continue;
        const QString src = entry.absoluteFilePath();
        if (entry.isSymLink()) {
            reportMigrationFailure(
                root, QStringLiteral(
                    "refusing to migrate symlinked engine dir %1")
                    .arg(src),
                error, kEngineMigrationFailedReport);
            return false;
        }

        // A stray FILE at a persona-shaped name is not an engine
        // store — leave it untouched and say so rather than rename
        // unknown bytes into a profile tree.
        if (!entry.isDir()) {
            qWarning() << "BrowserPaths: skipping non-directory engine"
                          " entry" << src;
            continue;
        }

        // Qt materializes the name-keyed default dir at profile
        // construction even though the real roots are overridden —
        // the same residue defaultDataPathFor() documents for
        // containers.  An EMPTY src is that residue, not a payload:
        // drop it unconditionally so a booted-once tree never
        // false-collides with the already-relocated store.
        if (QDir(src).entryList(
                QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden
                | QDir::System).isEmpty()) {
            QDir(src).removeRecursively();
            continue;
        }

        // A persona nobody can address — no profile dir, no vault —
        // is leftover from a deleted profile.  Leave it in place and
        // say so; fabricating a fresh profiles/<id> for it would
        // masquerade orphan data as a live store.
        const QString profileDir =
            root + QLatin1String("/profiles/") + id;
        const QString hold = engineVaultHold(root, id);
        const QString vaultDir =
            root + QLatin1String("/profiles/") + id
            + QLatin1String(".vault");
        if (id != QLatin1String("default") && hold.isEmpty()
            && !QFile::exists(profileDir) && !QFile::exists(vaultDir)
            && !QFile::exists(root + QLatin1Char('/') + id
                              + QLatin1String(".vault"))) {
            qWarning() << "BrowserPaths: leaving unowned engine dir"
                          " in place (no profile or vault for id)"
                       << src;
            continue;
        }

        QString dst;
        if (!hold.isEmpty()) {
            dst = hold;
        } else {
            dst = profileDir + QLatin1String("/QtWebEngine");
        }
        // The destination chain gets the same lstat discipline —
        // profiles/, profiles/<id>, the vault dir and engine-hold are
        // all components a planted link could redirect through.
        if (pathChainHasLink(dst, root)) {
            reportMigrationFailure(
                root, QStringLiteral(
                    "refusing to migrate %1 into a symlinked"
                    " destination %2").arg(src, dst),
                error, kEngineMigrationFailedReport);
            return false;
        }
        if (QFile::exists(dst)) {
            reportMigrationFailure(
                root, QStringLiteral(
                    "%1 exists in both QtWebEngine/ and %2")
                    .arg(name, dst),
                error, kEngineMigrationFailedReport);
            return false;
        }
        if (!QDir().mkpath(QFileInfo(dst).absolutePath())) {
            reportMigrationFailure(
                root, QStringLiteral("cannot create %1")
                    .arg(QFileInfo(dst).absolutePath()),
                error, kEngineMigrationFailedReport);
            return false;
        }
        if (!QFile::rename(src, dst)) {
            reportMigrationFailure(
                root, QStringLiteral("rename %1 -> %2 failed")
                    .arg(src, dst),
                error, kEngineMigrationFailedReport);
            return false;
        }
    }

    // The legacy cache dirs (<cache root>/QtWebEngine/arora*) are
    // disposable — a cache is rebuilt, never migrated; cross-filesystem
    // moves add nothing but failure modes.  A planted link is removed
    // as a link (QFile::remove never follows) rather than descended.
    const QString cacheEngineRoot =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
        + QLatin1String("/QtWebEngine");
    if (!QFileInfo(cacheEngineRoot).isSymLink()) {
        const QFileInfoList cacheEntries =
            QDir(cacheEngineRoot).entryInfoList(
                QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot
                | QDir::Hidden | QDir::System);
        for (const QFileInfo &entry : cacheEntries) {
            const QString name = entry.fileName();
            if (name != QLatin1String("arora")
                && !name.startsWith(QLatin1String("arora-")))
                continue;
            const QString path = entry.absoluteFilePath();
            if (entry.isSymLink()) {
                QFile::remove(path);
            } else if (!QDir(path).removeRecursively()) {
                qWarning() << "BrowserPaths: could not remove legacy"
                              " engine cache" << path;
            }
        }
    } else {
        qWarning() << "BrowserPaths: refusing to clean a symlinked"
                      " cache QtWebEngine dir" << cacheEngineRoot;
    }

    QFile::remove(root + QLatin1Char('/')
                  + QLatin1String(kEngineMigrationFailedReport));
    return true;
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
