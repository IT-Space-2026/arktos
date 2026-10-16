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
#include "profilestore.h"

#include <qatomic.h>
#include <qcoreapplication.h>
#include <qdatetime.h>
#include <qdir.h>
#include <qdiriterator.h>
#include <qevent.h>
#include <qeventloop.h>
#include <qfile.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qsavefile.h>
#include <qthread.h>
#include <qtimer.h>

// Runs one engine call on a worker thread so seal/unseal of a large
// profile never stalls the UI.  Progress and completion cross back to
// the GUI thread as queued signals; cancel is a polled flag — the
// FFI's void progress callback can't interrupt mid-call, so a cancel
// that lands late still discards the result honestly.
class VaultOperationThread : public QThread
{
    Q_OBJECT

public:
    typedef std::function<ProfileVaultEngine::Status(
        const ProfileVaultEngine::ProgressFn &,
        const ProfileVaultEngine::CancelFn &)> WorkFn;

    VaultOperationThread(const WorkFn &fn, QObject *parent = nullptr)
        : QThread(parent)
        , m_fn(fn)
    {
        m_cancel.storeRelease(0);
    }

    void cancel() { m_cancel.storeRelease(1); }

signals:
    void progressed(quint64 done, quint64 total);
    void finished(int status);

protected:
    void run() override
    {
        ProfileVaultEngine::ProgressFn progress =
            [this](quint64 done, quint64 total) {
                emit progressed(done, total);
            };
        ProfileVaultEngine::CancelFn cancel =
            [this]() { return m_cancel.loadAcquire() != 0; };
        emit finished(int(m_fn(progress, cancel)));
    }

private:
    WorkFn m_fn;
    QAtomicInt m_cancel;
};

// -------------------------------------------------------------------
// The real engine — compiles in automatically once PVAULT01's
// rustcore FFI lands on this lane: the merged rustcore.h declares
// rv_* next to the RC_VAULT_ALGO_* constants, and that define is the
// probe (a merge needs no .pri change for the upgrade).  Until then
// instance() runs NullVaultEngine and every entry point reports the
// honest unavailable state.
// -------------------------------------------------------------------

#if defined(ARORA_RUSTCORE)
#include <rustcore.h>
#endif
#if defined(RC_VAULT_ALGO_AES256_GCM) && !defined(ARORA_RUSTCORE_VAULT)
#define ARORA_RUSTCORE_VAULT 1
#endif

#ifdef ARORA_RUSTCORE_VAULT

class RustCoreVaultEngine : public ProfileVaultEngine
{
public:
    bool available() const override { return true; }

    static Status map(RcStatus status)
    {
        switch (status) {
        case RC_OK: return EngineOk;
        case RC_WRONG_PASSWORD: return WrongPassword;
        case RC_TAMPERED: return Tampered;
        case RC_CORRUPT: return Corrupt;
        case RC_UNSUPPORTED_ALGO: return UnsupportedAlgo;
        case RC_IO: return IoError;
        default: return IoError;
        }
    }

    Status seal(const QString &dirPath, const QByteArray &password,
                int algo, const QString &outBlobPath,
                const ProgressFn &progress, const CancelFn &) override
    {
        ProgressCtx ctx;
        ctx.fn = progress;
        return map(rv_seal(
            QFile::encodeName(dirPath).constData(),
            reinterpret_cast<const uint8_t *>(password.constData()),
            size_t(password.size()), uint8_t(algo),
            QFile::encodeName(outBlobPath).constData(),
            progress ? &RustCoreVaultEngine::progressTrampoline
                     : nullptr,
            progress ? &ctx : nullptr));
    }

    Status unseal(const QString &blobPath, const QByteArray &password,
                  const QString &outDirPath, const ProgressFn &progress,
                  const CancelFn &) override
    {
        ProgressCtx ctx;
        ctx.fn = progress;
        return map(rv_unseal(
            QFile::encodeName(blobPath).constData(),
            reinterpret_cast<const uint8_t *>(password.constData()),
            size_t(password.size()),
            QFile::encodeName(outDirPath).constData(),
            progress ? &RustCoreVaultEngine::progressTrampoline
                     : nullptr,
            progress ? &ctx : nullptr));
    }

    Status verify(const QString &blobPath,
                  const QByteArray &password) override
    {
        return map(rv_verify(
            QFile::encodeName(blobPath).constData(),
            reinterpret_cast<const uint8_t *>(password.constData()),
            size_t(password.size())));
    }

    Status rewrap(const QString &blobPath, const QByteArray &oldPassword,
                  const QByteArray &newPassword) override
    {
        return map(rv_rewrap(
            QFile::encodeName(blobPath).constData(),
            reinterpret_cast<const uint8_t *>(oldPassword.constData()),
            size_t(oldPassword.size()),
            reinterpret_cast<const uint8_t *>(newPassword.constData()),
            size_t(newPassword.size())));
    }

    QByteArray inspectJson(const QString &blobPath) override
    {
        char *json = rv_inspect(
            QFile::encodeName(blobPath).constData());
        if (!json)
            return QByteArray();
        QByteArray out = QByteArray::fromRawData(json, int(strlen(json)));
        QByteArray copy = out;
        rc_string_free(json);
        return copy;
    }

private:
    struct ProgressCtx { ProgressFn fn; };
    static void progressTrampoline(uint64_t done, uint64_t total,
                                   void *userdata)
    {
        ProgressCtx *ctx = static_cast<ProgressCtx *>(userdata);
        if (ctx && ctx->fn)
            ctx->fn(done, total);
    }
};
#endif // ARORA_RUSTCORE_VAULT

// -------------------------------------------------------------------

ProfileVault::ProfileVault(ProfileStore *store,
                           ProfileVaultEngine *engine,
                           QObject *parent)
    : QObject(parent)
    , m_store(store)
    , m_engine(engine)
    , m_idleTimer(nullptr)
    , m_lastActivityMs(0)
    , m_sessionEnded(false)
{
    // Profiles restored with an idle deadline set need the activity
    // filter from the start, not only after a mid-session edit.
    installActivityFilter();
}

ProfileVault::~ProfileVault()
{
    // An op outliving its vault would write to freed state — the app
    // only tears the singleton down at exit, but tests destroy
    // instances, so wait out any stragglers.
    for (VaultOperationThread *thread : m_running) {
        thread->wait();
        delete thread;
    }
    delete m_engine;
}

ProfileVault *ProfileVault::instance()
{
    static ProfileVault *vault = nullptr;
    if (!vault) {
        vault = new ProfileVault(ProfileStore::instance(),
                                 defaultEngine(), qApp);
    }
    return vault;
}

ProfileVaultEngine *ProfileVault::defaultEngine()
{
#ifdef ARORA_RUSTCORE_VAULT
    return new RustCoreVaultEngine;
#else
    return new NullVaultEngine;
#endif
}

ProfileStore *ProfileVault::store() const
{
    return m_store;
}

bool ProfileVault::engineAvailable() const
{
    return m_engine && m_engine->available();
}

QString ProfileVault::blobPath(const QString &profileDir)
{
    return profileDir + QLatin1String(".arv");
}

QString ProfileVault::lockMarkerPath(const QString &profileDir)
{
    return profileDir + QLatin1String(".vault.lock");
}

QString ProfileVault::confPath(const QString &profileDir)
{
    return profileDir + QLatin1String(".vault.conf");
}

QString ProfileVault::dirtyMarkerPath(const QString &profileDir)
{
    return profileDir + QLatin1String("/unlocked.dirty");
}

void ProfileVault::removeSidecars(const QString &profileDir)
{
    QFile::remove(blobPath(profileDir));
    QFile::remove(lockMarkerPath(profileDir));
    QFile::remove(confPath(profileDir));
}

QString ProfileVault::dirPath(const QString &profileId) const
{
    return m_store->profileDir(profileId);
}

bool ProfileVault::isVaulted(const QString &profileId) const
{
    const QString dir = dirPath(profileId);
    return QFile::exists(confPath(dir)) || QFile::exists(blobPath(dir));
}

bool ProfileVault::isLocked(const QString &profileId) const
{
    return QFile::exists(blobPath(dirPath(profileId)));
}

bool ProfileVault::isUnlocked(const QString &profileId) const
{
    const QString dir = dirPath(profileId);
    return isVaulted(profileId) && !isLocked(profileId)
           && QFile::exists(dir);
}

QByteArray ProfileVault::confJson(const QString &profileId) const
{
    QFile file(confPath(dirPath(profileId)));
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    return file.readAll();
}

int ProfileVault::algo(const QString &profileId) const
{
    // Prefer the lock marker (written at seal, always reflects the
    // blob) then the conf's configured pick.
    for (const QString &path : { lockMarkerPath(dirPath(profileId)),
                                 confPath(dirPath(profileId)) }) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const int algo = QJsonDocument::fromJson(file.readAll())
            .object().value(QLatin1String("algo")).toInt(0);
        if (algo == AlgoAes256Gcm || algo == AlgoThreefishXex)
            return algo;
    }
    return AlgoAes256Gcm;
}

QByteArray ProfileVault::inspect(const QString &profileId) const
{
    if (!m_engine || !isLocked(profileId))
        return QByteArray();
    return m_engine->inspectJson(blobPath(dirPath(profileId)));
}

bool ProfileVault::writeConf(const QString &profileId, int algoId,
                             bool autoLockOn, int idleMinutes)
{
    QJsonObject root;
    root.insert(QLatin1String("algo"),
                algoId == AlgoThreefishXex ? AlgoThreefishXex
                                           : AlgoAes256Gcm);
    root.insert(QLatin1String("autoLock"), autoLockOn);
    root.insert(QLatin1String("idleLockMinutes"), qMax(0, idleMinutes));
    QSaveFile file(confPath(dirPath(profileId)));
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    if (!file.commit())
        return false;
    emit vaultChanged(profileId);
    return true;
}

bool ProfileVault::autoLock(const QString &profileId) const
{
    const QJsonObject conf =
        QJsonDocument::fromJson(confJson(profileId)).object();
    // Spec default: ON for vaulted profiles.
    return conf.value(QLatin1String("autoLock")).toBool(true);
}

void ProfileVault::setAutoLock(const QString &profileId, bool on)
{
    writeConf(profileId, algo(profileId), on,
              idleLockMinutes(profileId));
}

int ProfileVault::idleLockMinutes(const QString &profileId) const
{
    const QJsonObject conf =
        QJsonDocument::fromJson(confJson(profileId)).object();
    return conf.value(QLatin1String("idleLockMinutes")).toInt(0);
}

void ProfileVault::setIdleLockMinutes(const QString &profileId,
                                      int minutes)
{
    writeConf(profileId, algo(profileId), autoLock(profileId),
              minutes);
    if (minutes > 0)
        installActivityFilter();
}

int ProfileVault::openWindowCount(const QString &profileId) const
{
    // Same MPROF03 seam as ProfileManagerDialog::openWindowCount —
    // until per-window binding lands, the active (default) profile
    // owns every window and the rest own none.
    BrowserApplication *application = BrowserApplication::instance();
    if (!application || profileId != ProfileStore::activeProfileId())
        return 0;
    return application->mainWindows().count();
}

bool ProfileVault::canLockNow(const QString &profileId,
                              QString *reason) const
{
    const QString why = [this, &profileId]() -> QString {
        if (!engineAvailable())
            return tr("The vault encryption engine is not available "
                      "in this build.");
        if (isBusy(profileId))
            return tr("A vault operation is already running on this "
                      "profile.");
        if (isLocked(profileId))
            return tr("This profile is already locked.");
        if (!isVaulted(profileId))
            return tr("Set a password for this profile in the "
                      "Profile Manager first.");
        if (openWindowCount(profileId) > 0)
            return tr("Close every window running this profile before "
                      "locking it — or turn on \"Lock when the last "
                      "window closes\" in Preferences.");
        return QString();
    }();
    if (reason)
        *reason = why;
    return why.isEmpty();
}

bool ProfileVault::hasSessionPassword(const QString &profileId) const
{
    return m_sessionPasswords.contains(profileId);
}

void ProfileVault::cacheSessionPassword(const QString &profileId,
                                        const QByteArray &password)
{
    m_sessionPasswords.insert(profileId, password);
}

QByteArray ProfileVault::sessionPassword(const QString &profileId) const
{
    return m_sessionPasswords.value(profileId);
}

void ProfileVault::clearSessionPassword(const QString &profileId)
{
    QByteArray &password = m_sessionPasswords[profileId];
    // Best-effort scrub before the buffer is dropped — the real
    // zeroizing happens inside the FFI's Zeroizing wraps.
    if (!password.isEmpty())
        password.fill(0);
    m_sessionPasswords.remove(profileId);
}

void ProfileVault::setPassword(const QString &profileId,
                               const QByteArray &password, int algoId)
{
    if (password.isEmpty()
        || !ProfileStore::isValidProfileId(profileId))
        return;
    cacheSessionPassword(profileId, password);
    writeConf(profileId, algoId, autoLock(profileId),
              idleLockMinutes(profileId));
}

bool ProfileVault::startOperation(const QString &profileId,
                                  int operation,
                                  VaultOperationThread *thread)
{
    if (!engineAvailable() || m_running.contains(profileId)
        || !ProfileStore::isValidProfileId(profileId)) {
        delete thread;
        return false;
    }
    m_running.insert(profileId, thread);
    m_runningOp.insert(profileId, operation);
    connect(thread, &VaultOperationThread::progressed, this,
            [this, profileId](quint64 done, quint64 total) {
        emit progress(profileId, done, total);
    });
    connect(thread, &VaultOperationThread::finished, this,
            [this, profileId, operation](int status) {
        operationDone(profileId, operation, status);
    });
    thread->start();
    return true;
}

bool ProfileVault::startSeal(const QString &profileId,
                             const QByteArray &password)
{
    const QString dir = dirPath(profileId);
    if (password.isEmpty() || isLocked(profileId)
        || !QFile::exists(dir))
        return false;
    // "Never lock a profile with open windows" — the explicit quit /
    // auto-lock paths set m_sessionEnded first, which is exactly the
    // last-window-closed moment the rule protects.
    if (!m_sessionEnded && openWindowCount(profileId) > 0)
        return false;
    const int algoId = algo(profileId);
    const QString blob = blobPath(dir);
    ProfileVaultEngine *engine = m_engine;
    return startOperation(profileId, SealOperation,
        new VaultOperationThread(
            [engine, dir, password, algoId, blob](
                const ProfileVaultEngine::ProgressFn &progress,
                const ProfileVaultEngine::CancelFn &cancel) {
            if (cancel())
                return ProfileVaultEngine::Canceled;
            ProfileVaultEngine::Status status = engine->seal(
                dir, password, algoId, blob, progress, cancel);
            if (status == ProfileVaultEngine::EngineOk && cancel())
                status = ProfileVaultEngine::Canceled;
            return status;
        }, this));
}

bool ProfileVault::startUnseal(const QString &profileId,
                               const QByteArray &password)
{
    const QString dir = dirPath(profileId);
    const QString blob = blobPath(dir);
    if (password.isEmpty() || !QFile::exists(blob))
        return false;
    // PVAULT02's honesty rule: never unseal over existing plaintext —
    // a non-empty leftover dir is crash residue the recovery flow
    // owns; an empty one is inert and the unseal fills it.
    const QDir existing(dir);
    if (existing.exists()
        && !existing.entryList(QDir::NoDotAndDotDot
                               | QDir::AllEntries).isEmpty())
        return false;
    m_pendingPasswords.insert(profileId, password);
    ProfileVaultEngine *engine = m_engine;
    return startOperation(profileId, UnsealOperation,
        new VaultOperationThread(
            [engine, dir, password, blob](
                const ProfileVaultEngine::ProgressFn &progress,
                const ProfileVaultEngine::CancelFn &cancel) {
            ProfileVaultEngine::Status status = engine->unseal(
                blob, password, dir, progress, cancel);
            if (status == ProfileVaultEngine::EngineOk && cancel())
                status = ProfileVaultEngine::Canceled;
            return status;
        }, this));
}

bool ProfileVault::startRewrap(const QString &profileId,
                               const QByteArray &oldPassword,
                               const QByteArray &newPassword)
{
    const QString blob = blobPath(dirPath(profileId));
    if (oldPassword.isEmpty() || newPassword.isEmpty()
        || !QFile::exists(blob))
        return false;
    m_pendingPasswords.insert(profileId, newPassword);
    ProfileVaultEngine *engine = m_engine;
    return startOperation(profileId, RewrapOperation,
        new VaultOperationThread(
            [engine, blob, oldPassword, newPassword](
                const ProfileVaultEngine::ProgressFn &,
                const ProfileVaultEngine::CancelFn &cancel) {
            if (cancel())
                return ProfileVaultEngine::Canceled;
            return engine->rewrap(blob, oldPassword, newPassword);
        }, this));
}

void ProfileVault::requestCancel(const QString &profileId)
{
    VaultOperationThread *thread = m_running.value(profileId);
    if (thread)
        thread->cancel();
}

bool ProfileVault::isBusy(const QString &profileId) const
{
    return m_running.contains(profileId);
}

int ProfileVault::runningOperation(const QString &profileId) const
{
    return m_runningOp.value(profileId, NoOperation);
}

void ProfileVault::operationDone(const QString &profileId,
                                 int operation, int status)
{
    VaultOperationThread *thread = m_running.take(profileId);
    m_runningOp.remove(profileId);
    if (thread) {
        thread->wait();
        thread->deleteLater();
    }

    const QByteArray pendingPassword =
        m_pendingPasswords.take(profileId);
    const QString dir = dirPath(profileId);
    const QString blob = blobPath(dir);
    QString message;
    switch (operation) {
    case SealOperation:
        if (status == ProfileVaultEngine::Canceled) {
            // Honest rollback: the plaintext dir was never touched —
            // discard the produced blob and stay unlocked.
            QFile::remove(blob);
        } else if (status == ProfileVaultEngine::EngineOk) {
            // PVAULT02 upgrades this to its secure-erase pass; until
            // then a plain recursive delete is what stands between
            // blob and plaintext.
            if (!QDir(dir).removeRecursively()) {
                QFile::remove(blob);
                status = ProfileVaultEngine::IoError;
                message = tr("The vault was written but the plaintext "
                             "folder could not be removed — the "
                             "profile was left unlocked.");
                break;
            }
            // The lock marker carries the algo for password-less
            // display (same trick as rv_inspect's header read); the
            // conf persists the prefs a first seal may never have
            // written.
            QJsonObject marker;
            marker.insert(QLatin1String("algo"), algo(profileId));
            marker.insert(QLatin1String("sealedAt"),
                QDateTime::currentDateTimeUtc()
                    .toString(Qt::ISODate));
            QSaveFile lockFile(lockMarkerPath(dir));
            if (lockFile.open(QIODevice::WriteOnly)) {
                lockFile.write(QJsonDocument(marker)
                    .toJson(QJsonDocument::Compact));
                lockFile.commit();
            }
            if (!QFile::exists(confPath(dir)))
                writeConf(profileId, algo(profileId), true, 0);
            clearSessionPassword(profileId);
        }
        break;
    case UnsealOperation:
        if (status == ProfileVaultEngine::Canceled) {
            // Discard the restored plaintext — the blob stays and the
            // profile remains honestly locked.
            QDir(dir).removeRecursively();
        } else if (status == ProfileVaultEngine::EngineOk) {
            QFile::remove(blob);
            QFile::remove(lockMarkerPath(dir));
            cacheSessionPassword(profileId, pendingPassword);
            armDirtyMarker(profileId);
        } else {
            // Engines are structurally no-partial, but don't trust
            // the seam: any stray output dir goes away so the profile
            // never sits half-unpacked.
            if (QFile::exists(dir))
                QDir(dir).removeRecursively();
        }
        break;
    case RewrapOperation:
        if (status == ProfileVaultEngine::EngineOk)
            // The new password opens the blob now — keep it for the
            // session so a later lock never re-prompts.
            cacheSessionPassword(profileId, pendingPassword);
        break;
    }
    if (message.isEmpty())
        message = statusText(status);
    emit operationFinished(profileId, operation, status, message);
    emit vaultChanged(profileId);
}

QString ProfileVault::statusText(int status)
{
    switch (status) {
    case ProfileVaultEngine::EngineOk:
        return QString();
    case ProfileVaultEngine::WrongPassword:
        return tr("The password is incorrect.");
    case ProfileVaultEngine::Tampered:
        return tr("The vault integrity check failed — the data may "
                  "be corrupted or tampered with.");
    case ProfileVaultEngine::Corrupt:
        return tr("The vault file is damaged or unreadable.");
    case ProfileVaultEngine::UnsupportedAlgo:
        return tr("This vault uses a cipher this build does not "
                  "support.");
    case ProfileVaultEngine::IoError:
        return tr("The vault could not be read or written.");
    case ProfileVaultEngine::EngineUnavailable:
        return tr("The vault encryption engine is not available in "
                  "this build.");
    case ProfileVaultEngine::Canceled:
        return tr("The operation was canceled.");
    }
    return tr("The vault operation failed.");
}

void ProfileVault::armDirtyMarker(const QString &profileId)
{
    const QString dir = dirPath(profileId);
    if (!QFile::exists(dir))
        return;
    QSaveFile file(dirtyMarkerPath(dir));
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(
            QJsonObject{
                { QLatin1String("unlockedAt"),
                  QDateTime::currentDateTimeUtc()
                      .toString(Qt::ISODate) } })
            .toJson(QJsonDocument::Compact));
        file.commit();
    }
}

void ProfileVault::clearDirtyMarker(const QString &profileId)
{
    QFile::remove(dirtyMarkerPath(dirPath(profileId)));
}

QStringList ProfileVault::dirtyProfiles() const
{
    QStringList dirty;
    for (const ProfileContext &context : m_store->profiles()) {
        const QString dir = dirPath(context.id);
        // unlocked.dirty + no sealed blob = the last run died with
        // this profile left in plaintext.
        if (QFile::exists(dirtyMarkerPath(dir))
            && !QFile::exists(blobPath(dir)))
            dirty.append(context.id);
    }
    return dirty;
}

int ProfileVault::sealBlocking(const QString &profileId,
                               const QByteArray &password,
                               QWidget *dialogParent)
{
    Q_UNUSED(dialogParent);
    if (!startSeal(profileId, password))
        return isLocked(profileId)
            ? int(ProfileVaultEngine::EngineOk)
            : int(ProfileVaultEngine::EngineUnavailable);
    int result = ProfileVaultEngine::EngineUnavailable;
    QEventLoop loop;
    QObject::connect(this, &ProfileVault::operationFinished, &loop,
        [&](const QString &id, int op, int status, const QString &) {
            if (id == profileId && op == SealOperation) {
                result = status;
                loop.quit();
            }
        });
    loop.exec();
    return result;
}

void ProfileVault::noteWindowClosed(const QString &profileId,
                                    QWidget *dialogParent)
{
    Q_UNUSED(dialogParent);
    if (m_sessionEnded || !engineAvailable()
        || openWindowCount(profileId) > 0
        || !isUnlocked(profileId) || !autoLock(profileId)
        || !hasSessionPassword(profileId))
        return;
    // Last window of a vaulted profile — seal before the state can
    // outlive its password.  Blocking: the alternative (async seal
    // racing process exit) is how plaintext gets left behind.
    sealBlocking(profileId, sessionPassword(profileId), dialogParent);
}

void ProfileVault::noteSessionEnding()
{
    if (m_sessionEnded)
        return;
    m_sessionEnded = true;
    if (m_idleTimer)
        m_idleTimer->stop();

    for (const ProfileContext &context : m_store->profiles()) {
        const QString id = context.id;
        if (!isUnlocked(id))
            continue;
        if (engineAvailable() && autoLock(id)
            && hasSessionPassword(id)) {
            const int status =
                sealBlocking(id, sessionPassword(id), nullptr);
            if (status == ProfileVaultEngine::EngineOk)
                continue;   // sealed — no dirty state survives
            // A failed seal keeps the marker so the next boot can
            // say the plaintext was left behind honestly.
            continue;
        }
        // Clean exit with the profile intentionally left unlocked —
        // the marker only exists to catch unclean deaths.
        clearDirtyMarker(id);
    }

    // Session passwords die with the session — best-effort scrub.
    const QList<QString> ids = m_sessionPasswords.keys();
    for (const QString &id : ids)
        clearSessionPassword(id);
}

void ProfileVault::installActivityFilter()
{
    if (m_idleTimer)
        return;
    if (!qApp)
        return;
    m_lastActivityMs = QDateTime::currentMSecsSinceEpoch();
    qApp->installEventFilter(this);
    m_idleTimer = new QTimer(this);
    m_idleTimer->setInterval(30000);
    connect(m_idleTimer, &QTimer::timeout,
            this, &ProfileVault::checkIdleDeadlines);
    m_idleTimer->start();
}

bool ProfileVault::eventFilter(QObject *watched, QEvent *event)
{
    switch (event->type()) {
    case QEvent::KeyPress:
    case QEvent::MouseButtonPress:
    case QEvent::Wheel:
    case QEvent::TouchBegin:
        m_lastActivityMs = QDateTime::currentMSecsSinceEpoch();
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

void ProfileVault::checkIdleDeadlines()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const ProfileContext &context : m_store->profiles()) {
        const QString id = context.id;
        const int minutes = idleLockMinutes(id);
        if (minutes <= 0 || !isUnlocked(id)
            || !hasSessionPassword(id))
            continue;
        if (now - m_lastActivityMs < qint64(minutes) * 60000)
            continue;
        // The open-window rule still stands — an idle profile whose
        // windows are open locks when they close instead (auto-lock).
        if (openWindowCount(id) > 0)
            continue;
        sealBlocking(id, sessionPassword(id), nullptr);
    }
}

#include "profilevault.moc"
