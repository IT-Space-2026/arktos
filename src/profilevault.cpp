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

#include "profilevault.h"

#include "browserapplication.h"
#include "browserpaths.h"
#include "profilesettings.h"

#include <qcoreapplication.h>
#include <qdatetime.h>
#include <qdebug.h>
#include <qdir.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qsavefile.h>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

#ifdef ARORA_RUSTCORE
#include <rustcore.h>
#endif

namespace {

// Top-level names excluded from the sealed tree and held aside as
// "<name>-hold/" inside the vault dir: "cache" is the spec'd
// disposable exclusion (stays unsealed by design); "profiles" keeps
// the pre-MPROF data-root layout from swallowing sibling profiles and
// their vaults — a no-op once MPROF01 makes profiles/<id> the dir.
const char *const kExcludedNames[] = { "cache", "profiles" };

void secureZero(QByteArray &bytes)
{
    volatile unsigned char *p =
        static_cast<volatile unsigned char *>(
            static_cast<void *>(bytes.data()));
    for (qsizetype i = 0; i < bytes.size(); ++i)
        p[i] = 0;
}

// The MPROF01 profile-id contract (BrowserPaths::isProfileId on the
// merged tree): lowercase slug, 1..64 chars.  Duplicate — whoever
// merges second deletes their copy, same rule as ProfilePaths.
bool idIsSlug(const QString &id)
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

QString vaultPath(const QString &vaultDir, const QString &name)
{
    return vaultDir + QLatin1Char('/') + name;
}

// Registered lazily on first unlock — runs the shutdown seal no
// matter which teardown ordering the platform takes.
void shutdownSealRoutine()
{
    ProfileVault::instance()->sealUnlockedProfilesAtShutdown();
}

#ifdef ARORA_RUSTCORE
bool dirHasContents(const QString &path)
{
    QDir dir(path);
    return !dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot
                          | QDir::Hidden | QDir::System)
                .isEmpty();
}
#endif

// Single zero overwrite + fsync, then unlink.  This is the honest
// "no readable plaintext at the path" guarantee — on SSD/CoW media
// wear-leveling can keep stale blocks alive and there is no
// userspace fix for that; see the header comment.
bool secureEraseFile(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadWrite)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    const qint64 size = file.size();
    static const char zeros[65536] = {0};
    qint64 left = size;
    while (left > 0) {
        const qint64 chunk = qMin<qint64>(left, sizeof(zeros));
        if (file.write(zeros, chunk) != chunk) {
            if (error)
                *error = file.errorString();
            return false;
        }
        left -= chunk;
    }
    file.flush();
#ifdef Q_OS_UNIX
    ::fsync(file.handle());
#endif
    file.close();
    return QFile::remove(path);
}

bool secureEraseTree(const QString &path, QString *error)
{
    const QFileInfo info(path);
    // isSymLink is lstat-based — it stays true for a dangling link,
    // where exists() (target-based) is already false.  Check it
    // before exists() or a link whose target was erased first would
    // be left behind as readable plaintext metadata.
    if (info.isSymLink())
        return QFile::remove(path);
    if (!info.exists())
        return true;
    if (info.isDir()) {
        QDir dir(path);
        const QFileInfoList entries = dir.entryInfoList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden
            | QDir::System);
        for (const QFileInfo &entry : entries) {
            if (!secureEraseTree(entry.absoluteFilePath(), error))
                return false;
        }
        return dir.rmdir(path);
    }
    return secureEraseFile(path, error);
}

#ifdef ARORA_RUSTCORE
bool secureEraseTreeContents(const QString &path, QString *error)
{
    QDir dir(path);
    const QFileInfoList entries = dir.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden
        | QDir::System);
    for (const QFileInfo &entry : entries) {
        if (!secureEraseTree(entry.absoluteFilePath(), error))
            return false;
    }
    return true;
}
#endif

bool writeMarker(const QString &path, const QByteArray &contents,
                 QString *error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    file.write(contents);
    if (!file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

#ifdef ARORA_RUSTCORE

QByteArray encoded(const QString &path)
{
    return QFile::encodeName(path);
}

QString rustError()
{
    char *raw = rc_last_error_message();
    QString message = QString::fromUtf8(raw ? raw : "");
    if (raw)
        rc_string_free(raw);
    return message;
}

#endif

} // namespace

ProfileVault::ProfileVault(QObject *parent)
    : QObject(parent)
{
}

ProfileVault *ProfileVault::instance()
{
    static ProfileVault *self = new ProfileVault;
    return self;
}

bool ProfileVault::isAvailable()
{
#ifdef ARORA_RUSTCORE
    return true;
#else
    return false;
#endif
}

QString ProfileVault::normalizedId(const QString &profileId)
{
    QString id = profileId.isEmpty()
        ? ProfileSettings::defaultId() : profileId;
    if (!idIsSlug(id)) {
        qWarning() << "ProfileVault: refusing non-slug profile id"
                   << profileId << "- using default";
        id = ProfileSettings::defaultId();
    }
    return id;
}

QString ProfileVault::profileDirFor(const QString &profileId)
{
    const QString id = normalizedId(profileId);
    // MPROF01 retargets this seam: post-merge "default" also lives at
    // profiles/default/ and this folds into profileDataDir(id).
    if (id == QLatin1String("default"))
        return QDir::cleanPath(BrowserPaths::dataFilePath(QString()));
    return ProfilePaths::dirFor(id);
}

QString ProfileVault::vaultDirFor(const QString &profileId)
{
    const QString id = normalizedId(profileId);
    const QFileInfo dir(profileDirFor(id));
    return dir.dir().filePath(id + QLatin1String(".vault"));
}

QString ProfileVault::errorMessage() const
{
    return m_lastError;
}

ProfileVault::Error ProfileVault::fail(Error error,
                                       const QString &message) const
{
    m_lastError = message;
    if (error != NoError)
        qWarning() << "ProfileVault:" << message;
    return error;
}

ProfileVault::State ProfileVault::state(const QString &profileId) const
{
    const QString vault = vaultDirFor(profileId);
    const bool conf =
        QFile::exists(vaultPath(vault, QLatin1String("vault.conf")));
    const bool blob =
        QFile::exists(vaultPath(vault, QLatin1String("vault.arv")));
    const bool lock =
        QFile::exists(vaultPath(vault, QLatin1String("vault.lock")));
    const bool dirty =
        QFile::exists(vaultPath(vault, QLatin1String("unlocked.dirty")));
    if (lock)
        return Locked;
    if (blob)
        return SealInterrupted;
    if (dirty)
        return DirtyUnlocked;
    if (conf)
        return Unlocked;
    return NoVault;
}

bool ProfileVault::isLocked(const QString &profileId) const
{
    return state(profileId) == Locked;
}

QStringList ProfileVault::dirtyProfiles() const
{
    QStringList out;
    // Artifact dirs are siblings of the profile dir: the default
    // profile's vault sits next to the data root, non-default ids
    // under profiles/ — pre-MPROF both locations exist.
    const QString defaultDir = profileDirFor(QString());
    const QStringList roots = {
        QFileInfo(defaultDir).dir().absolutePath(),
        defaultDir + QLatin1String("/profiles")
    };
    for (const QString &root : roots) {
        const QStringList vaults = QDir(root).entryList(
            QStringList() << QLatin1String("*.vault"),
            QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &name : vaults) {
            const QString id = name.left(name.size() - 6); // ".vault"
            if (!idIsSlug(id) || out.contains(id))
                continue;
            if (state(id) == DirtyUnlocked)
                out.append(id);
        }
    }
    return out;
}

#ifdef ARORA_RUSTCORE

ProfileVault::Error ProfileVault::mapStatus(int status,
                                            const QString &fallback) const
{
    const QString detail = rustError();
    const QString message =
        detail.isEmpty() ? fallback : fallback + QLatin1String(": ") + detail;
    switch (status) {
    case RC_OK:
        return fail(NoError, QString());
    case RC_WRONG_PASSWORD:
        return fail(WrongPassword, message);
    case RC_TAMPERED:
        return fail(Tampered, message);
    case RC_CORRUPT:
        return fail(Corrupt, message);
    case RC_UNSUPPORTED_ALGO:
        return fail(UnsupportedAlgo, message);
    case RC_EMPTY_PASSPHRASE:
        return fail(EmptyPassword, message);
    default:
        return fail(Io, message);
    }
}

void ProfileVault::progressTrampoline(uint64_t done, uint64_t total,
                                      void *userdata)
{
    // Fires synchronously on the FFI caller's thread — the struct is
    // stack-owned by seal()/unseal(), receivers wanting the GUI
    // thread use a queued connection.
    struct Ctx { ProfileVault *self; QString id; };
    const Ctx *ctx = static_cast<const Ctx *>(userdata);
    emit ctx->self->progress(ctx->id, done, total);
}

#endif

bool ProfileVault::hasOpenWindows(const QString &id) const
{
#ifdef AUTOTESTS
    if (m_testGate >= 0)
        return m_testGate != 0;
#endif
    // MPROF03 retargets: pre-binding every window belongs to the
    // default profile, so a non-default id can never have one open.
    if (normalizedId(id) != ProfileSettings::defaultId())
        return false;
    BrowserApplication *app = BrowserApplication::instance();
    return app && !app->mainWindows().isEmpty();
}

bool ProfileVault::holdExcluded(const QString &id, bool hold,
                                QString *error)
{
    const QString dir = profileDirFor(id);
    const QString vault = vaultDirFor(id);
    for (const char *name : kExcludedNames) {
        const QString live = dir + QLatin1Char('/') + QLatin1String(name);
        const QString held =
            vaultPath(vault, QLatin1String(name) + QLatin1String("-hold"));
        const bool liveExists = QFile::exists(live);
        const bool heldExists = QFile::exists(held);
        if (hold) {
            // A stale hold means an earlier cycle never restored —
            // the live tree wins, the disposable hold goes.
            if (heldExists && !secureEraseTree(held, error))
                return false;
            if (liveExists && !QDir().rename(live, held)) {
                if (error)
                    *error = QStringLiteral("cannot move %1 aside")
                                 .arg(live);
                return false;
            }
        } else {
            if (liveExists) {
                // The restored tree already carries the name (an old
                // vault may have packed it) — drop the held copy.
                if (heldExists && !secureEraseTree(held, error))
                    return false;
            } else if (heldExists && !QDir().rename(held, live)) {
                if (error)
                    *error = QStringLiteral("cannot restore %1")
                                 .arg(held);
                return false;
            }
        }
    }
    return true;
}

bool ProfileVault::readConf(const QString &id, QVariantMap *conf) const
{
    QFile file(vaultPath(vaultDirFor(id), QLatin1String("vault.conf")));
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QJsonDocument doc =
        QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject())
        return false;
    *conf = doc.object().toVariantMap();
    return true;
}

bool ProfileVault::writeConf(const QString &id, const QVariantMap &conf,
                             QString *error) const
{
    QDir().mkpath(vaultDirFor(id));
    const QJsonDocument doc(QJsonObject::fromVariantMap(conf));
    return writeMarker(
        vaultPath(vaultDirFor(id), QLatin1String("vault.conf")),
        doc.toJson(QJsonDocument::Compact), error);
}

void ProfileVault::cachePassword(const QString &id,
                                 const QByteArray &password)
{
    dropPassword(id);
    m_passwords.insert(id, password);
    // Cover the teardown path no matter which destructor ordering the
    // platform takes — the same idempotent call ~BrowserApplication
    // and quitBrowser() make directly.
    static bool routineRegistered = false;
    if (!routineRegistered) {
        routineRegistered = true;
        qAddPostRoutine(&shutdownSealRoutine);
    }
}

void ProfileVault::dropPassword(const QString &id)
{
    auto it = m_passwords.find(id);
    if (it != m_passwords.end()) {
        secureZero(it.value());
        m_passwords.erase(it);
    }
}

ProfileVault::Error ProfileVault::seal(const QString &profileId,
                                      const QByteArray &password,
                                      quint8 algo)
{
    const QString id = normalizedId(profileId);
    m_lastError.clear();
#ifndef ARORA_RUSTCORE
    Q_UNUSED(password);
    Q_UNUSED(algo);
    return fail(Unavailable,
                QStringLiteral("profile vault needs a rustcore build"));
#else
    if (password.isEmpty())
        return fail(EmptyPassword,
                    QStringLiteral("empty vault password"));
    const QString vault = vaultDirFor(id);
    const QString lockPath =
        vaultPath(vault, QLatin1String("vault.lock"));
    if (QFile::exists(lockPath))
        return fail(AlreadyLocked,
                    QStringLiteral("profile %1 is already locked").arg(id));
    if (QFile::exists(vaultPath(vault, QLatin1String("vault.arv"))))
        return fail(Io, QStringLiteral(
            "profile %1 has an interrupted seal — restore with unseal "
            "(allowOverwrite) or remove the vault first").arg(id));
    if (hasOpenWindows(id))
        return fail(HasOpenWindows, QStringLiteral(
            "refusing to lock profile %1 while it has open windows")
            .arg(id));

    const QString dir = profileDirFor(id);
    QDir().mkpath(vault);
    QString ioError;
    if (!holdExcluded(id, true, &ioError))
        return fail(Io, ioError);
    // The blob must be fully committed before any plaintext is erased
    // — a crash between them reads SealInterrupted and recovers
    // through unseal(allowOverwrite) off the intact vault.
    struct Ctx { ProfileVault *self; QString id; } ctx = { this, id };
    const QByteArray d = encoded(dir);
    const QByteArray out = encoded(
        vaultPath(vault, QLatin1String("vault.arv")));
    const RcStatus st = rv_seal(
        d.constData(),
        reinterpret_cast<const uint8_t *>(password.constData()),
        size_t(password.size()), algo, out.constData(),
        &ProfileVault::progressTrampoline, &ctx);
    if (st != RC_OK) {
        QString restoreError;
        holdExcluded(id, false, &restoreError);
        return mapStatus(
            st, QStringLiteral("seal failed for profile %1").arg(id));
    }

    if (!secureEraseTreeContents(dir, &ioError))
        return fail(Io, QStringLiteral(
            "profile %1 sealed but plaintext erase failed: %2")
            .arg(id, ioError));

    QFile::remove(vaultPath(vault, QLatin1String("unlocked.dirty")));
    const QString sealedAt =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    writeMarker(lockPath,
                QStringLiteral("profile=%1 sealed_at=%2\n")
                    .arg(id, sealedAt).toUtf8(), &ioError);

    QVariantMap conf;
    readConf(id, &conf); // keep user keys (auto_lock) across re-seals
    conf.insert(QLatin1String("version"), 1);
    conf.insert(QLatin1String("profile"), id);
    conf.insert(QLatin1String("locked"), true);
    conf.insert(QLatin1String("sealed_at"), sealedAt);
    if (!conf.contains(QLatin1String("auto_lock")))
        conf.insert(QLatin1String("auto_lock"), true);
    QStringList excluded;
    for (const char *name : kExcludedNames)
        excluded.append(QLatin1String(name));
    conf.insert(QLatin1String("excluded"), excluded);
    // Record the params the blob actually carries — rv_inspect reads
    // them out of the sealed header rather than trusting defaults.
    char *raw = rv_inspect(out.constData());
    if (raw) {
        const QJsonDocument doc =
            QJsonDocument::fromJson(QByteArray(raw));
        rc_string_free(raw);
        const QVariantMap meta = doc.object().toVariantMap();
        conf.insert(QLatin1String("algo"), meta.value(
                        QLatin1String("algo"), int(algo)));
        conf.insert(QLatin1String("algo_name"), meta.value(
                        QLatin1String("algo_name")));
        QVariantMap kdf;
        kdf.insert(QLatin1String("name"),
                   meta.value(QLatin1String("kdf"),
                              QLatin1String("argon2id")));
        kdf.insert(QLatin1String("memory_kib"),
                   meta.value(QLatin1String("memory_kib")));
        kdf.insert(QLatin1String("passes"),
                   meta.value(QLatin1String("passes")));
        kdf.insert(QLatin1String("lanes"),
                   meta.value(QLatin1String("lanes")));
        conf.insert(QLatin1String("kdf"), kdf);
    } else {
        conf.insert(QLatin1String("algo"), int(algo));
    }
    if (!writeConf(id, conf, &ioError))
        return fail(Io, ioError);

    dropPassword(id); // locked — custody ends
    emit profileLocked(id);
    return fail(NoError, QString());
#endif
}

ProfileVault::Error ProfileVault::lockProfile(const QString &profileId)
{
    const QString id = normalizedId(profileId);
    m_lastError.clear();
    if (state(id) == Locked)
        return fail(AlreadyLocked,
                    QStringLiteral("profile %1 is already locked").arg(id));
    const QByteArray password = m_passwords.value(id);
    if (password.isEmpty())
        return fail(NoPassword, QStringLiteral(
            "no cached password for profile %1 — the caller must "
            "collect one and call seal()").arg(id));
    quint8 algo = 0x01;
    QVariantMap conf;
    if (readConf(id, &conf))
        algo = quint8(conf.value(QLatin1String("algo"), 1).toUInt());
    const Error error = seal(id, password, algo);
    return error;
}

ProfileVault::Error ProfileVault::unseal(const QString &profileId,
                                        const QByteArray &password,
                                        bool allowOverwrite)
{
    const QString id = normalizedId(profileId);
    m_lastError.clear();
#ifndef ARORA_RUSTCORE
    Q_UNUSED(password);
    Q_UNUSED(allowOverwrite);
    return fail(Unavailable,
                QStringLiteral("profile vault needs a rustcore build"));
#else
    if (password.isEmpty())
        return fail(EmptyPassword,
                    QStringLiteral("empty vault password"));
    const QString vault = vaultDirFor(id);
    const QString blobPath =
        vaultPath(vault, QLatin1String("vault.arv"));
    if (!QFile::exists(blobPath)) {
        if (QFile::exists(vaultPath(vault, QLatin1String("vault.conf")))
            || QFile::exists(vaultPath(vault, QLatin1String("vault.lock"))))
            return fail(Io, QStringLiteral(
                "profile %1 is vault-registered but vault.arv is "
                "missing").arg(id));
        return fail(NotLocked,
                    QStringLiteral("profile %1 has no vault").arg(id));
    }

    const QString dir = profileDirFor(id);
    const QString park =
        vaultPath(vault, QLatin1String("pre-unlock"));
    QString ioError;
    // Recover or retire a parked tree from an earlier interrupted
    // overwrite: live plaintext wins, an empty destination means the
    // previous attempt never committed and the parked copy is the
    // only plaintext left.
    if (QFile::exists(park)) {
        if (dirHasContents(dir)) {
            if (!secureEraseTree(park, &ioError))
                return fail(Io, ioError);
        } else {
            QDir(dir).removeRecursively();
            if (!QDir().rename(park, dir))
                return fail(Io, QStringLiteral(
                    "cannot recover parked plaintext for %1").arg(id));
        }
    }

    bool parked = false;
    if (dirHasContents(dir)) {
        if (!allowOverwrite)
            return fail(PlaintextPresent, QStringLiteral(
                "refusing to unseal over existing plaintext in %1 "
                "without confirmation").arg(dir));
        if (!QDir().rename(dir, park))
            return fail(Io, QStringLiteral(
                "cannot park existing plaintext for %1").arg(id));
        parked = true;
    }

    struct Ctx { ProfileVault *self; QString id; } ctx = { this, id };
    const QByteArray d = encoded(dir);
    // Two-pass in the crate: the whole payload MAC verifies before a
    // byte of plaintext lands, so a rejection leaves zero artifacts.
    const QByteArray blob = encoded(blobPath);
    const RcStatus st = rv_unseal(
        blob.constData(),
        reinterpret_cast<const uint8_t *>(password.constData()),
        size_t(password.size()), d.constData(),
        &ProfileVault::progressTrampoline, &ctx);
    if (st != RC_OK) {
        if (parked && !QFile::exists(dir)
            && !QDir().rename(park, dir))
            qWarning() << "ProfileVault: unseal failed AND the parked"
                          " plaintext could not be restored — it is"
                          " still at" << park;
        return mapStatus(
            st, QStringLiteral("unseal failed for profile %1").arg(id));
    }

    holdExcluded(id, false, &ioError);
    if (QFile::exists(park))
        secureEraseTree(park, &ioError);

    // Marker swap: lock first so a crash reads SealInterrupted (blob
    // intact) rather than a falsely-clean Unlocked.
    QFile::remove(vaultPath(vault, QLatin1String("vault.lock")));
    QFile::remove(blobPath);
    const QString unlockedAt =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    writeMarker(vaultPath(vault, QLatin1String("unlocked.dirty")),
                QStringLiteral("profile=%1 unlocked_at=%2\n")
                    .arg(id, unlockedAt).toUtf8(), &ioError);
    QVariantMap conf;
    if (readConf(id, &conf)) {
        conf.insert(QLatin1String("locked"), false);
        conf.insert(QLatin1String("unlocked_at"), unlockedAt);
        writeConf(id, conf, &ioError);
    }
    cachePassword(id, password); // custody for the auto-lock re-seal
    emit profileUnlocked(id);
    return fail(NoError, QString());
#endif
}

ProfileVault::Error ProfileVault::verify(const QString &profileId,
                                        const QByteArray &password)
{
    const QString id = normalizedId(profileId);
    m_lastError.clear();
#ifndef ARORA_RUSTCORE
    Q_UNUSED(password);
    return fail(Unavailable,
                QStringLiteral("profile vault needs a rustcore build"));
#else
    const QString blob =
        vaultPath(vaultDirFor(id), QLatin1String("vault.arv"));
    if (!QFile::exists(blob))
        return fail(state(id) == NoVault ? NotLocked : Io,
                    QStringLiteral("no vault blob for profile %1").arg(id));
    const QByteArray b = encoded(blob);
    return mapStatus(
        rv_verify(b.constData(),
                  reinterpret_cast<const uint8_t *>(password.constData()),
                  size_t(password.size())),
        QStringLiteral("verify failed for profile %1").arg(id));
#endif
}

ProfileVault::Error ProfileVault::changePassword(
    const QString &profileId, const QByteArray &oldPassword,
    const QByteArray &newPassword)
{
    const QString id = normalizedId(profileId);
    m_lastError.clear();
#ifndef ARORA_RUSTCORE
    Q_UNUSED(oldPassword);
    Q_UNUSED(newPassword);
    return fail(Unavailable,
                QStringLiteral("profile vault needs a rustcore build"));
#else
    const QString blob =
        vaultPath(vaultDirFor(id), QLatin1String("vault.arv"));
    if (!QFile::exists(blob))
        return fail(NotLocked,
                    QStringLiteral("no vault blob for profile %1").arg(id));
    const QByteArray b = encoded(blob);
    const RcStatus st = rv_rewrap(
        b.constData(),
        reinterpret_cast<const uint8_t *>(oldPassword.constData()),
        size_t(oldPassword.size()),
        reinterpret_cast<const uint8_t *>(newPassword.constData()),
        size_t(newPassword.size()));
    return mapStatus(
        st, QStringLiteral("password change failed for profile %1")
            .arg(id));
#endif
}

QVariantMap ProfileVault::inspect(const QString &profileId)
{
    QVariantMap out;
    const QString id = normalizedId(profileId);
    out.insert(QLatin1String("profile"), id);
    out.insert(QLatin1String("state"), int(state(id)));
    out.insert(QLatin1String("vault_dir"), vaultDirFor(id));
#ifdef ARORA_RUSTCORE
    const QString blob =
        vaultPath(vaultDirFor(id), QLatin1String("vault.arv"));
    if (QFile::exists(blob)) {
        const QByteArray b = encoded(blob);
        if (char *raw = rv_inspect(b.constData())) {
            const QJsonDocument doc =
                QJsonDocument::fromJson(QByteArray(raw));
            rc_string_free(raw);
            const QVariantMap meta = doc.object().toVariantMap();
            for (auto it = meta.constBegin(); it != meta.constEnd(); ++it)
                out.insert(it.key(), it.value());
        }
    }
#endif
    return out;
}

bool ProfileVault::autoLock(const QString &profileId) const
{
    QVariantMap conf;
    if (!readConf(normalizedId(profileId), &conf))
        return true;
    return conf.value(QLatin1String("auto_lock"), true).toBool();
}

void ProfileVault::setAutoLock(const QString &profileId, bool enabled)
{
    const QString id = normalizedId(profileId);
    QVariantMap conf;
    readConf(id, &conf);
    conf.insert(QLatin1String("version"), 1);
    conf.insert(QLatin1String("profile"), id);
    conf.insert(QLatin1String("auto_lock"), enabled);
    QString error;
    if (!writeConf(id, conf, &error))
        qWarning() << "ProfileVault:" << error;
}

void ProfileVault::disarmDirty(const QString &profileId)
{
    QFile::remove(vaultPath(vaultDirFor(profileId),
                            QLatin1String("unlocked.dirty")));
}

void ProfileVault::checkForDirtyProfiles()
{
    const QStringList dirty = dirtyProfiles();
    for (const QString &id : dirty) {
        // Never silent: the plaintext is sitting unsealed because the
        // last run died before the auto-lock re-seal could run.
        qWarning() << "ProfileVault: profile" << id
                   << "was left unlocked by an unclean shutdown — its"
                      "plaintext data is on disk unsealed; offer a"
                      "re-seal (the residual-window risk is real)";
        emit dirtyDetected(id);
    }
}

void ProfileVault::sealUnlockedProfilesAtShutdown()
{
    if (!isAvailable())
        return;
    const QList<QString> ids = m_passwords.keys();
    for (const QString &id : ids) {
        const State s = state(id);
        if (s != DirtyUnlocked && s != Unlocked)
            continue;
        if (autoLock(id)) {
            const Error error = lockProfile(id);
            if (error != NoError)
                qWarning() << "ProfileVault: auto-lock of profile" << id
                           << "failed:" << errorMessage()
                           << "— plaintext remains on disk";
        } else {
            // Clean exit with auto-lock off: acknowledged plaintext,
            // not crash debris — disarm the dirty marker.
            disarmDirty(id);
        }
    }
    for (auto it = m_passwords.begin(); it != m_passwords.end(); ++it)
        secureZero(it.value());
    m_passwords.clear();
}

#ifdef AUTOTESTS
void ProfileVault::resetForTests()
{
    for (auto it = m_passwords.begin(); it != m_passwords.end(); ++it)
        secureZero(it.value());
    m_passwords.clear();
    m_testGate = -1;
}

void ProfileVault::setWindowGateForTests(bool hasOpenWindows)
{
    m_testGate = hasOpenWindows ? 1 : 0;
}

void ProfileVault::clearWindowGateForTests()
{
    m_testGate = -1;
}
#endif
