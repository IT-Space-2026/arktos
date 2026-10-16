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

#include <qobject.h>
#include <qbytearray.h>
#include <qhash.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qvariantmap.h>

#include <stdint.h>

class QTimer;

// PVAULT02: the Qt-side profile vault engine — orchestrates the
// rustcore "ARV1" FFI (PVAULT01's rv_seal/rv_unseal/rv_verify/
// rv_inspect/rv_rewrap) into the lock/unlock lifecycle the UI drives.
//
// Artifact layout — everything lives in a "<id>.vault/" dir that is a
// SIBLING of the profile dir, so the sealed tree never contains vault
// artifacts and rv_unseal's empty-destination requirement always
// holds:
//
//   <id>.vault/vault.arv      sealed blob (rv_seal out, rv_unseal in)
//   <id>.vault/vault.conf     JSON registry entry: algo + argon2id
//                             params + locked flag + auto_lock +
//                             sealed_at + the exclusion list
//   <id>.vault/vault.lock     lock marker — presence means Locked
//   <id>.vault/unlocked.dirty armed while the profile is unlocked; a
//                             boot that finds it WITHOUT vault.arv
//                             means we died with plaintext live
//   <id>.vault/cache-hold/    excluded dir held aside while locked
//   <id>.vault/profiles-hold/ same, for the nested profiles/ dir
//   <id>.vault/pre-unlock/    overwrite-unseal parking slot
//
// Exclusions from the sealed tree (top-level names):
//   "cache"     — spec'd: disposable; it stays on disk unsealed in
//                 the hold and is restored on unlock.  Honest caveat:
//                 a locked profile's cache is NOT encrypted — anyone
//                 reading the disk sees it.  Wiping it instead is a
//                 policy decision for PVAULT04.
//   "profiles"  — pre-MPROF the data root doubles as the profile dir
//                 AND the container for other profiles' dirs+vaults;
//                 sealing it would swallow them (and erase their
//                 vaults).  Post-MPROF the profile dir IS
//                 profiles/<id> and the exclusion is a harmless no-op.
//
// VSEC01 boundary amendment — the vault seals the APP-SIDE store
// tree only.  The engine's own persistence never enters the sealed
// set: the named QWebEngineProfile for persona <id> stores cookies,
// localStorage/IndexedDB, service workers, the HTTP disk cache and
// visited-link state in its own QtWebEngine/arora[-<id>]/ dir at the
// data root, and that dir is neither under profiles/<id> nor in the
// exclusion list — it simply isn't in scope.  A "locked" profile
// therefore still leaves every Chromium-side record on disk in
// cleartext; sealing is an app-store guarantee, not a browsing-data
// guarantee.  Closing that needs setPersistentStoragePath()/
// setCachePath() relocated inside the sealed tree (plus migration of
// existing dirs and a story for cache-held exclusions) — tracked as
// follow-up task VSEC02.
//
// Secure erase is ONE zero overwrite + fsync per file, then unlink.
// On SSD/CoW media wear-leveling and snapshots mean old blocks may
// survive — the guarantee is "no readable plaintext at the path",
// not physical erasure; no fake multi-pass claim.
//
// Profile-id plumbing is the MPROF01 conditional-hook seam: today
// "default" resolves to the data root (exactly what profiles/default
// becomes when MPROF01 merges — whoever merges second folds
// profileDirFor into BrowserPaths::profileDataDir), other ids resolve
// through MPROF04's ProfilePaths::dirFor fallback.  A vault created
// now seals the whole data root including profiles/ internals held
// aside as above; MPROF01's migration owns relocating artifacts.
//
// Lock triggers: the menu lock action (PVAULT03) calls lockProfile();
// the shutdown path calls sealUnlockedProfilesAtShutdown() from
// ~BrowserApplication and quitBrowser() — under pre-MPROF03
// single-profile semantics "the profile's last window closed" IS the
// app going down.  A post-routine registered on first unlock covers
// teardown ordering either way.  Never lock a profile with open
// windows (seal refuses HasOpenWindows); never unseal over existing
// plaintext without allowOverwrite.
//
// VSEC01: with MPROF03 live the window gate is per-persona —
// BrowserApplication::newMainWindow fails closed on a Locked id,
// and the closing window's OWN bound id drives noteWindowClosed
// (the ambient persona is not a substitute).  "Never lock a profile
// with open windows" is enforced by openWindowCount() counting
// windows by their bound persona.
//
// Without CONFIG+=rustcore the class still compiles — isAvailable()
// is false and every operation returns Unavailable, so call sites
// need no #ifdef.
class ProfileVault : public QObject
{
    Q_OBJECT

public:
    // Local conditions plus the honest RcStatus split the FFI
    // produces — WrongPassword (wrap rejected) is never conflated
    // with Tampered (a MAC failed after a good unwrap).
    enum Error {
        NoError = 0,
        WrongPassword,
        Tampered,
        Corrupt,
        UnsupportedAlgo,
        EmptyPassword,
        NotLocked,        // no vault exists for this profile
        AlreadyLocked,    // seal() on an already-locked profile
        PlaintextPresent, // unseal over a non-empty dir w/o confirm
        HasOpenWindows,   // lock refused — profile still has windows
        NoPassword,       // re-lock with no cached/explicit password
        Io,               // filesystem or other FFI failure
        Unavailable,      // built without rustcore
        Canceled          // UI-side state — ops themselves can't be
                          // canceled mid-flight (the FFI is
                          // synchronous); produced when a wait
                          // dialog is dismissed
    };
    Q_ENUM(Error)

    // Cipher ids — mirror RC_VAULT_ALGO_* so dialogs can name the
    // choice without pulling the FFI header in.
    enum VaultAlgo {
        AlgoAes256Gcm = 0x01,     // AES-256-GCM + HMAC-SHA-512
        AlgoThreefishXex = 0x02   // Threefish-1024 XEX + HMAC-Skein-512
    };

    // Marker-derived truth (vault.conf is a record, not the state):
    //   lock marker              -> Locked
    //   vault.arv w/o marker     -> SealInterrupted (crash mid-seal;
    //                               blob intact, plaintext partial)
    //   dirty marker w/o blob    -> DirtyUnlocked (died unlocked —
    //                               plaintext live, nothing sealed)
    //   conf, no markers         -> Unlocked (clean plaintext)
    //   nothing                  -> NoVault
    enum State {
        NoVault = 0,
        Locked,
        Unlocked,
        DirtyUnlocked,
        SealInterrupted
    };
    Q_ENUM(State)

    // Leaky app-lifetime singleton — never parented, so the shutdown
    // seal can run from any destructor ordering.
    static ProfileVault *instance();
    static bool isAvailable();

    static QString normalizedId(const QString &profileId);
    // The profile dir sealed/unsealed for this id (created on demand).
    static QString profileDirFor(const QString &profileId);
    // "<id>.vault/" sibling dir holding all artifacts (NOT created).
    static QString vaultDirFor(const QString &profileId);

    State state(const QString &profileId = QString()) const;
    bool isLocked(const QString &profileId = QString()) const;
    // Ids whose unlocked.dirty is armed with no sealed blob — the
    // "offer re-seal" set (crash-dirty recovery).
    QStringList dirtyProfiles() const;

    // Last failure's human-readable detail for the most recent op.
    QString errorMessage() const;

    // First seal creates the registry entry; re-seal after unlock uses
    // the recorded algo.  Progress fires on the calling thread.
    Error seal(const QString &profileId, const QByteArray &password,
               quint8 algo = 0x01);
    // Re-seal using the password cached by unseal() — the lock path.
    Error lockProfile(const QString &profileId = QString());
    // Verify (header + full payload MAC, fail closed) then unpack.
    // A non-empty destination refuses with PlaintextPresent unless
    // allowOverwrite — the parked plaintext is only erased after the
    // new tree lands intact.
    Error unseal(const QString &profileId, const QByteArray &password,
                 bool allowOverwrite = false);
    // Password + authenticity check without unpacking.
    Error verify(const QString &profileId, const QByteArray &password);
    // Cheap rv_rewrap password change — Locked vaults only.
    Error changePassword(const QString &profileId,
                         const QByteArray &oldPassword,
                         const QByteArray &newPassword);
    // rv_inspect JSON + state/path for UI display (UNVERIFIED fields).
    QVariantMap inspect(const QString &profileId = QString());

    // Registry flag: re-seal automatically when the app shuts down
    // with the profile unlocked (default on — unlock is per-session).
    bool autoLock(const QString &profileId = QString()) const;
    void setAutoLock(const QString &profileId, bool enabled);
    // Acknowledge plaintext on a clean shutdown without sealing —
    // clears the armed dirty marker for this id.
    void disarmDirty(const QString &profileId);
    // PVAULT04: Delete-Profile backend — wipes every vault artifact
    // for the id (blob, registry, markers, held exclusions, parking
    // slot) WITHOUT needing the password.  Owner-host honesty: the
    // local operator can always delete their own data — a locked
    // profile's blob is ciphertext either way, so demanding the
    // password before deletion would be lockout theater, not
    // protection.  Refuses while the profile has open windows.  The
    // live plaintext tree (unlocked / dirty-crash survivors) is NOT
    // touched — the caller's ordinary profile-dir removal covers it.
    // Works without rustcore: artifacts from a rust-enabled build
    // still have to be deletable on a no-rust one.  No-op success
    // when the id has no vault.
    Error wipeVault(const QString &profileId);

    // Boot-time crash-dirty scan — qWarning plus dirtyDetected() per
    // hit; the "Re-seal now" offer UI (PVAULT03/04) connects here.
    void checkForDirtyProfiles();
    // The auto-lock trigger: seal every profile unlocked this session
    // that has auto_lock on; disarm the dirty marker for the rest.
    // Called at the tail of ~BrowserApplication and quitBrowser().
    void sealUnlockedProfilesAtShutdown();

    // ---- PVAULT03 merge surface ----------------------------------
    // The ui lane's facade entry points, folded onto this engine.
    // vault.conf exists — a password vaulted this profile, sealed or
    // not.
    bool isVaulted(const QString &profileId) const;
    // Vaulted and currently open in plaintext.
    bool isUnlocked(const QString &profileId) const;
    // Configured cipher id from vault.conf (AlgoAes256Gcm default).
    quint8 algo(const QString &profileId) const;
    // The sealed blob's path for display — no existence check.
    static QString blobPathFor(const QString &profileId);

    // Windows currently bound to this id (MPROF03 live binding).
    int openWindowCount(const QString &profileId) const;
    // The Lock gate — `reason` carries the blocking cause for
    // tooltips: engine missing, not vaulted, already locked, an
    // interrupted seal, or windows still open.
    bool canLockNow(const QString &profileId,
                    QString *reason = nullptr) const;

    // Session password custody (set by unseal()/setPassword(), wiped
    // at seal/shutdown) — lock paths re-seal without re-prompting.
    bool hasSessionPassword(const QString &profileId) const;
    QByteArray sessionPassword(const QString &profileId) const;
    // Set-password entry: caches the session password and records
    // the algo pick in vault.conf — nothing is sealed yet, the dir
    // stays live.
    Error setPassword(const QString &profileId,
                      const QByteArray &password, quint8 algo);

    // Idle lock in minutes; 0 (default) means never.  The
    // m_idleTimer/eventFilter pair enforces it once installed.
    int idleLockMinutes(const QString &profileId) const;
    void setIdleLockMinutes(const QString &profileId, int minutes);

    // PVAULT03 hook: a window on `profileId` went away — seal when
    // the state + auto_lock pref say so and no windows remain on the
    // id.  Never disarms the dirty marker: the session may continue.
    void noteWindowClosed(const QString &profileId);

    // Honest user-facing text for an Error.
    static QString statusText(int error);

#ifdef AUTOTESTS
    // Drops every cached password + the window-gate override.
    void resetForTests();
    // Overrides the open-window check (default: BrowserApplication's
    // window list on "default").  std::function is not moc-safe, so
    // the override is a plain tri-state flag.
    void setWindowGateForTests(bool hasOpenWindows);
    void clearWindowGateForTests();
#endif

signals:
    // (done,total) — bytes; total is the crate's payload estimate.
    void progress(const QString &profileId, quint64 done, quint64 total);
    void profileLocked(const QString &profileId);
    void profileUnlocked(const QString &profileId);
    void dirtyDetected(const QString &profileId);
    // Vault-relevant state flipped for the profile — repaint
    // triggers for the manager/menu (PVAULT03 merge surface).
    void vaultChanged(const QString &profileId);

private slots:
    void checkIdleDeadlines();

private:
    explicit ProfileVault(QObject *parent = nullptr);

    Error fail(Error error, const QString &message) const;
    bool hasOpenWindows(const QString &id) const;
    bool holdExcluded(const QString &id, bool hold, QString *error);
    bool readConf(const QString &id, QVariantMap *conf) const;
    bool writeConf(const QString &id, const QVariantMap &conf,
                   QString *error) const;
    void cachePassword(const QString &id, const QByteArray &password);
    void dropPassword(const QString &id);
    bool eventFilter(QObject *watched, QEvent *event) override;
    void installIdleTracking();
#ifdef ARORA_RUSTCORE
    Error mapStatus(int status, const QString &fallback) const;
    static void progressTrampoline(uint64_t done, uint64_t total,
                                   void *userdata);
#endif

    // Passwords of profiles unlocked this session — custody for the
    // auto-lock re-seal.  Wiped on lock/shutdown.
    QHash<QString, QByteArray> m_passwords;
    mutable QString m_lastError;
    QTimer *m_idleTimer = nullptr;
    qint64 m_lastActivityMs = 0;
#ifdef AUTOTESTS
    int m_testGate = -1; // -1 none, 0 no windows, 1 windows open
#endif
};

#endif // PROFILEVAULT_H
