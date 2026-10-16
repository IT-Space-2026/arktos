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

#ifndef PROFILEVAULT_H
#define PROFILEVAULT_H

#include <qbytearray.h>
#include <qhash.h>
#include <qobject.h>
#include <qstring.h>
#include <qstringlist.h>

#include <functional>

class QTimer;
class QWidget;

class ProfileStore;
class VaultOperationThread;

// PVAULT03: profile vault UX facade + engine seam.
//
// PVAULT01's rustcore FFI (rv_seal/rv_unseal/rv_verify/rv_inspect/
// rv_rewrap — "ARV1" chunked sealed archives of a profile directory)
// has merged to master but not into this lane's build, so the real
// engine here is a seam: ProfileVaultEngine is a vtable of exactly
// the FFI's operations, and the production instance() wires
// RustCoreVaultEngine when the header declares it (the merge flips
// ARORA_RUSTCORE_VAULT on in rustcore.pri) and NullVaultEngine —
// every call fails EngineUnavailable — until then.  Tests inject a
// reversible stub engine through the constructor, so every dialog,
// state transition and marker leg is exercised today.
//
// On-disk contract (the PVAULT02 engine task keeps these paths when
// it lands — the UI already speaks them):
//   profiles/<id>/            plaintext profile dir — present only
//                             while the profile is unsealed
//   profiles/<id>.arv         the sealed vault blob (engine output)
//   profiles/<id>.vault.lock  marker written while sealed — carries
//                             the algo id so a locked profile's
//                             cipher is known before any password
//   profiles/<id>.vault.conf  JSON prefs: algo, autoLock,
//                             idleLockMinutes — exists once the
//                             profile is vaulted, sealed or not
//   profiles/<id>/unlocked.dirty
//                             armed the moment an unseal finishes
//                             and cleared at seal or at a clean
//                             session end — a survivor at boot means
//                             the last run died with the profile
//                             left in plaintext
//
// Passwords needed by seal (the FFI wraps the vault key with an
// Argon2id KEK derived from the password) come from the session
// password cache: set at unseal/set-password time, wiped on seal and
// at session end.  Auto-lock could never re-seal without it — nothing
// is ever written to disk.
class ProfileVaultEngine
{
public:
    // Mapped 1:1 onto the honest RcStatus distinctions PVAULT01's FFI
    // returns (RC_WRONG_PASSWORD / RC_TAMPERED / RC_CORRUPT /
    // RC_UNSUPPORTED_ALGO / RC_IO) plus UI-side states.
    enum Status {
        EngineOk = 0,
        WrongPassword,      // wrap rejected the password
        Tampered,           // MAC check failed
        Corrupt,            // malformed vault structure
        UnsupportedAlgo,    // algo_id this build can't read
        IoError,            // filesystem failure — see message
        EngineUnavailable,  // no crypto backend in this build
        Canceled            // abandoned at the caller's request
    };

    // Progress as (bytes_done, bytes_total) — the FFI fires it
    // synchronously on the calling thread; engines emit it from
    // whatever thread runs the operation.
    typedef std::function<void(quint64 done, quint64 total)> ProgressFn;
    // Polled between units of work; true means the caller wants out.
    // Engines that cannot interrupt mid-call still honor it before
    // and after the blocking span — the facade discards the result
    // either way, so cancel always rolls back honestly.
    typedef std::function<bool()> CancelFn;

    virtual ~ProfileVaultEngine() {}

    virtual bool available() const = 0;

    // Pack dirPath -> outBlobPath atomically (temp + rename); the
    // source dir is only read, never mutated — the facade owns the
    // plaintext cleanup after a kept seal.
    virtual Status seal(const QString &dirPath,
                        const QByteArray &password, int algo,
                        const QString &outBlobPath,
                        const ProgressFn &progress,
                        const CancelFn &cancel) = 0;
    // Verify + unpack blobPath into outDirPath — structurally
    // no-partial: a failure or a cancel leaves no plaintext behind.
    virtual Status unseal(const QString &blobPath,
                          const QByteArray &password,
                          const QString &outDirPath,
                          const ProgressFn &progress,
                          const CancelFn &cancel) = 0;
    // Password + authenticity check without unpacking.
    virtual Status verify(const QString &blobPath,
                          const QByteArray &password) = 0;
    // Cheap password change — re-wraps the vault key; the payload
    // never moves (PVAULT01's two-tier key schedule).
    virtual Status rewrap(const QString &blobPath,
                          const QByteArray &oldPassword,
                          const QByteArray &newPassword) = 0;
    // UNVERIFIED header summary JSON for display, empty on error.
    virtual QByteArray inspectJson(const QString &blobPath) = 0;
};

// The every-call-fails engine instance() uses until the rustcore FFI
// merges — honest EngineUnavailable, never a fake vault.
class NullVaultEngine : public ProfileVaultEngine
{
public:
    bool available() const override { return false; }
    Status seal(const QString &, const QByteArray &, int,
                const QString &, const ProgressFn &,
                const CancelFn &) override { return EngineUnavailable; }
    Status unseal(const QString &, const QByteArray &,
                  const QString &, const ProgressFn &,
                  const CancelFn &) override { return EngineUnavailable; }
    Status verify(const QString &, const QByteArray &) override
        { return EngineUnavailable; }
    Status rewrap(const QString &, const QByteArray &,
                  const QByteArray &) override { return EngineUnavailable; }
    QByteArray inspectJson(const QString &) override { return QByteArray(); }
};

class ProfileVault : public QObject
{
    Q_OBJECT

signals:
    // Vault-relevant on-disk state flipped for the profile — the
    // manager repaints, the menu re-evaluates the Lock item.
    void vaultChanged(const QString &profileId);
    // Bytes forwarded from the engine's progress callback.
    void progress(const QString &profileId, quint64 done, quint64 total);
    // `operation` is an Operation, `status` an engine Status; message
    // carries the honest detail (engine error or rollback note).
    void operationFinished(const QString &profileId, int operation,
                           int status, const QString &message);

public:
    enum Operation {
        NoOperation = 0,
        SealOperation,
        UnsealOperation,
        RewrapOperation
    };

    enum VaultAlgo {
        AlgoAes256Gcm = 0x01,     // AES-256-GCM + HMAC-SHA-512
        AlgoThreefishXex = 0x02   // Threefish-1024 XEX + HMAC-Skein-512
    };

    // The vault over `store`'s profiles/ tree, driven by `engine`
    // (taken over — owned).  Tests pass their own store + stub engine
    // for a fully hermetic tree.
    ProfileVault(ProfileStore *store, ProfileVaultEngine *engine,
                 QObject *parent = nullptr);
    ~ProfileVault();

    // qApp-owned singleton over ProfileStore::instance() and the real
    // engine seam (RustCoreVaultEngine when the FFI is compiled in,
    // NullVaultEngine until then).
    static ProfileVault *instance();

    // The seam switch — RustCoreVaultEngine under
    // ARORA_RUSTCORE_VAULT, NullVaultEngine otherwise.
    static ProfileVaultEngine *defaultEngine();

    ProfileStore *store() const;
    bool engineAvailable() const;

    // --- state -----------------------------------------------------
    // A password was set: vault.conf or a sealed blob exists.
    bool isVaulted(const QString &profileId) const;
    // Sealed at rest: the blob (and its lock marker) replaced the dir.
    bool isLocked(const QString &profileId) const;
    // Vaulted and currently open in plaintext.
    bool isUnlocked(const QString &profileId) const;
    // Configured cipher (AlgoAes256Gcm when unset/unknown).
    int algo(const QString &profileId) const;
    // Engine inspectJson() on the blob — unverified header fields.
    QByteArray inspect(const QString &profileId) const;

    // --- per-profile prefs (vault.conf) ----------------------------
    // "Lock when this profile's last window closes" — default ON for
    // vaulted profiles per spec.
    bool autoLock(const QString &profileId) const;
    void setAutoLock(const QString &profileId, bool on);
    // Idle lock in minutes; 0 (default) means never.
    int idleLockMinutes(const QString &profileId) const;
    void setIdleLockMinutes(const QString &profileId, int minutes);

    // --- windows ----------------------------------------------------
    // MPROF03 seam, same rule ProfileManagerDialog applies: on this
    // lane every open window counts against the active (default)
    // profile; other ids read 0 until per-window binding lands.
    int openWindowCount(const QString &profileId) const;
    // Spec gate: never lock a profile that still has windows.  The
    // blocking reason is returned for tooltips.
    bool canLockNow(const QString &profileId,
                    QString *reason = nullptr) const;

    // --- session password -------------------------------------------
    bool hasSessionPassword(const QString &profileId) const;
    // The cached session password — the UI passes it to startSeal so
    // lock paths never re-prompt inside a session.  Empty when none.
    QByteArray sessionPassword(const QString &profileId) const;
    // Set-password entry: records algo + enables vaulting in
    // vault.conf and caches the password for this session's first
    // seal.  Nothing is sealed yet — the dir is still live.
    void setPassword(const QString &profileId,
                     const QByteArray &password, int algo);

    // --- async operations (worker thread) ----------------------------
    // All three reject (return false) while an op is running on the
    // profile, the engine is unavailable, or the state can't take
    // them; progress/operationFinished still report a real run.
    bool startSeal(const QString &profileId, const QByteArray &password);
    bool startUnseal(const QString &profileId,
                     const QByteArray &password);
    // Change the password of a SEALED vault (rewrap — payload never
    // moves).  An unlocked vaulted profile just swaps its session
    // password + conf, so this call is for the locked case.
    bool startRewrap(const QString &profileId,
                     const QByteArray &oldPassword,
                     const QByteArray &newPassword);
    void requestCancel(const QString &profileId);
    bool isBusy(const QString &profileId) const;
    int runningOperation(const QString &profileId) const;

    // Honest user-facing text for an engine Status.
    static QString statusText(int status);

    // --- lifecycle hooks --------------------------------------------
    // Profiles whose unlocked.dirty marker survived the last run —
    // the crash-left-plaintext set the dirty-boot offer works on.
    QStringList dirtyProfiles() const;
    // A window on `profileId` was destroyed and the count may have
    // hit zero — fires the auto-lock seal when the pref and state say
    // so.  `dialogParent` anchors the progress dialog when the app is
    // still up; passing nullptr runs the seal silently (shutdown).
    void noteWindowClosed(const QString &profileId, QWidget *dialogParent);
    // Clean-session end (aboutToQuit / explicit quit): auto-locks what
    // the prefs ask for, then clears dirty markers for profiles the
    // user intentionally leaves unlocked.  A crashed run never calls
    // this — that's what makes the marker honest.
    void noteSessionEnding();

    // --- on-disk contract (statics so ProfileStore::deleteProfile can
    // wipe the sidecars without owning an engine) -------------------
    static QString blobPath(const QString &profileDir);
    static QString lockMarkerPath(const QString &profileDir);
    static QString confPath(const QString &profileDir);
    static QString dirtyMarkerPath(const QString &profileDir);
    // Removes blob + lock marker + conf (never the dirty marker — it
    // lives inside the dir the caller deletes itself).
    static void removeSidecars(const QString &profileDir);

private slots:
    void operationDone(const QString &profileId, int operation,
                       int status);
    void checkIdleDeadlines();

private:
    bool eventFilter(QObject *watched, QEvent *event) override;

    QString dirPath(const QString &profileId) const;
    QByteArray confJson(const QString &profileId) const;
    bool writeConf(const QString &profileId, int algo,
                   bool autoLock, int idleMinutes);
    void cacheSessionPassword(const QString &profileId,
                              const QByteArray &password);
    void clearSessionPassword(const QString &profileId);
    void armDirtyMarker(const QString &profileId);
    void clearDirtyMarker(const QString &profileId);
    // Shared runner for the three ops; startSeal/startUnseal/
    // startRewrap build the engine call.
    bool startOperation(const QString &profileId, int operation,
                        VaultOperationThread *thread);
    // Blocking seal used by auto-lock/quit — spins a local wait so
    // shutdown can't exit mid-write.
    int sealBlocking(const QString &profileId,
                     const QByteArray &password, QWidget *dialogParent);
    void installActivityFilter();

    ProfileStore *m_store;
    ProfileVaultEngine *m_engine;
    QHash<QString, VaultOperationThread *> m_running;
    QHash<QString, int> m_runningOp;
    QHash<QString, QByteArray> m_sessionPasswords;
    // The password an in-flight unseal/rewrap was started with —
    // promoted to the session cache on success, dropped on failure.
    QHash<QString, QByteArray> m_pendingPasswords;
    QTimer *m_idleTimer;
    qint64 m_lastActivityMs;
    bool m_sessionEnded;
};

#endif // PROFILEVAULT_H
