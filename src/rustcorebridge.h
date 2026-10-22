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

#ifndef RUSTCOREBRIDGE_H
#define RUSTCOREBRIDGE_H

#include <profilecontext.h>

#include <qobject.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qvariant.h>

struct AcCore;
struct AcHistory;
struct AcBookmarks;
struct AcPolicy;
struct AcSession;
struct AcIcons;
struct RcNotes;
struct RcSettings;
struct RcProfiles;

// Qt side of the rustcore change-callback -> Qt-signal bridge
// (RCORE01).  The crate invokes one C callback with a topic string;
// RustCoreBridge re-emits it as a queued storeChanged() signal so a
// Rust-side caller on any thread lands safely on the GUI thread.
//
// Topics so far: "credentials" (the rc_cred_* map changed),
// "bookmarks" and "history" (the RCORE02 stores), "autofill" (the
// RCORE05 record set), "settings" (RC02), "profiles" (RC03),
// "torctl" (RC14 — a control reply/event is queued for drain; the
// TorControl shell pulls them off the handle), "icons" (RC16 — the
// profile's favicon cache changed), "notes" (RC21 — the sidebar's
// per-persona note list changed).
// Compiled only under CONFIG+=rustcore (ARKTOS_RUSTCORE).
//
// RC07: the same object is also the app's AcCore — the facade root
// the arktos_core.h module handles derive from.  core() is the raw
// handle for engine-side consumers; the settings()/profiles()/
// history()/bookmarks()/policy() accessors mint profile-bound
// handles directly.  The facade's (module, profile_id) callback
// lane lands here as storeChangedFor() — emitted ALONGSIDE
// storeChanged(), never instead of it, so legacy listeners see the
// same traffic they always did.
class RustCoreBridge : public QObject
{
    Q_OBJECT

public:
    // Leaky app-lifetime singleton, created on demand — call only when
    // a QCoreApplication exists.
    static RustCoreBridge *instance();
    // True once instance() has minted the singleton — the check a
    // non-GUI caller makes before deciding instance() is safe (the
    // QObject's thread affinity was already pinned on the GUI
    // thread).  Call only when a QCoreApplication exists.
    static bool exists();
    // The bridge when this thread may use it: an existing singleton
    // from any thread, or a first mint on the GUI thread only (a
    // worker's first mint would pin the QObject's affinity wrong).
    // NULL when there is no QCoreApplication or ac_init failed —
    // callers fall back to the legacy rc_* spelling then.  Also NULL
    // when the app's data root moved after the core was minted
    // (test-mode renames, QStandardPaths toggles) — a minted handle's
    // per-op re-assert would keep writing the OLD root, while the
    // legacy lane re-anchors to the current one.
    static RustCoreBridge *serving();
    // True when dataRoot() no longer matches the root the core was
    // minted against — holders of cached facade handles (m_hist,
    // m_bm, the ProfileSettings store map) drop and remint on this.
    static bool rootDrifted();

    // RC07: the facade root — NULL only if ac_init itself failed (a
    // warning with ac_last_error() is logged at construction).
    AcCore *core() const { return m_core; }

    // Profile-bound module handles over the shared core.  An empty
    // profileId binds the default profile (global scope for
    // settings).  Caller releases with the matching ac_*_close (or
    // rc_settings_close/rc_profiles_close — same handle types); the
    // core outlives them all.
    RcSettings *settings(const QString &profileId = QString());
    RcProfiles *profiles();
    AcHistory *history(const QString &profileId = QString());
    AcBookmarks *bookmarks(const QString &profileId = QString());
    AcPolicy *policy(const QString &profileId = QString());
    AcSession *session(const QString &profileId = QString());
    AcIcons *icons(const QString &profileId = QString());
    // AcNotes — the facade typedef of RcNotes (arktos_core.h).
    RcNotes *notes(const QString &profileId = QString());

signals:
    void storeChanged(const QString &topic);
    // RC07: the scoped spelling — the bound profile that changed, or
    // empty for global-scope and unscoped (legacy-path) mutations.
    void storeChangedFor(const QString &topic, const QString &profileId);

private:
    explicit RustCoreBridge(QObject *parent = nullptr);
    static void trampoline(const char *topic, void *userdata);
    static void coreTrampoline(const char *module, const char *profileId,
                               void *userdata);
    AcCore *m_core = nullptr;
    QString m_root;
};

// Points the crate at the application data dir — the same
// idempotent call the SecureStore shim makes before every op, so
// test-mode data-dir switches propagate (RCORE02).
void rustCoreEnsureDataDir();

// MPROF02: points the crate at a SPECIFIC profile's dir —
// profiles/<id>/ instead of the active profile's.  The rust store is
// process-global: switching dirs drops its cached keys and the
// decoded credential/autofill maps (per rc_set_data_dir), and every
// DirCache re-stamps, so alternating profiles is correct but
// expensive — callers anchor per operation batch, never per page
// load.  An empty or non-slug id resolves to the default profile,
// matching BrowserPaths::profileDataDir.
void rustCoreEnsureDataDirFor(const QString &profileId);

// RC02: thin Qt wrapper over the rc_settings_* typed per-profile
// store — the QSettings-shaped surface RC08 swaps in for
// ProfileSettings' persistence.  The scope binds at construction:
// a profile slug opens <data root>/profiles/<id>/settings.json and
// auto-imports the legacy settings.ini on first open; an empty id
// opens the global scope (<data root>/settings.json).
//
// RC08: the handle is minted through the shared AcCore (ac_settings)
// whenever the bridge can serve — same rc_settings_open inside, but
// tracked against the app-lifetime root so a facade-minted handle
// stays bound when a test-mode root switch would strand an
// rc_opened one.  The rc_settings_open spelling remains the fallback
// for contexts the bridge cannot reach (no app instance yet, a
// non-GUI first touch, ac_init failure).
//
// Mirrors the C ABI 1:1: value() reconstructs a QVariant from the
// store's typed JSON (string/bool/int/double/bytes/string-list),
// setValue() maps the QVariant back, and QVariant-style conversion
// covers the ini-imported string scalars.
class RustCoreSettings
{
public:
    // Empty profileId selects the global scope.
    explicit RustCoreSettings(const QString &profileId = QString());
    ~RustCoreSettings();

    RustCoreSettings(const RustCoreSettings &) = delete;
    RustCoreSettings &operator=(const RustCoreSettings &) = delete;

    bool isValid() const;

    QVariant value(const QString &key,
                   const QVariant &defaultValue = QVariant()) const;
    void setValue(const QString &key, const QVariant &value);
    void remove(const QString &key);
    bool contains(const QString &key) const;
    // Keys under prefix (boundary match: prefix and prefix/…; an
    // empty prefix lists everything).
    QStringList allKeys(const QString &prefix = QString()) const;
    // QSettings::clear() — drop every key in the scope.
    void clear();
    void sync();

    // One-shot merge of a legacy QSettings ini at an explicit path —
    // the lane for the global QSettings file, which lives outside
    // the data root.  Profile scope keeps isProfileKey()-classified
    // keys only; global scope keeps the complement.
    bool importLegacyIni(const QString &iniPath);

    QString profileId() const;

private:
    RcSettings *m_h;
    QString m_profileId;
};

struct RcProfiles;

// RC03: thin Qt wrapper over the rc_profiles_* persona registry —
// the surface that replaces the ProfileRegistry/ProfileStore
// duplication on the rust path.  The data root binds at
// construction (empty → BrowserPaths::dataRoot(); tests pass a
// QTemporaryDir); profiles.json under it stays byte-compatible with
// the MPROF01 writer both ways.
//
// RC08: an empty dataRoot mints through the shared AcCore
// (ac_profiles — same rc_profiles_open inside, tracked against the
// app-lifetime root); an explicit root keeps the rc_profiles_open
// spelling so test fixtures can bind a QTemporaryDir the core never
// saw.
//
// Scope honesty: this is the STORE primitive layer.  The Qt-owned
// half of ProfileStore::deleteProfile — BrowserProfile::releasePersona
// (refuses while a window is bound) and ProfileVault::wipeVault — is
// NOT reproduced here; the RC08 adoption adapter composes those
// guards around deleteProfile() the way profilestore.cpp does today.
// Likewise the PNG decode/scale pipeline stays Qt — setAvatarName()
// only points the registry field at a basename the caller already
// wrote under profiles/<id>/.
class RustCoreProfiles
{
public:
    explicit RustCoreProfiles(const QString &dataRoot = QString());
    ~RustCoreProfiles();

    RustCoreProfiles(const RustCoreProfiles &) = delete;
    RustCoreProfiles &operator=(const RustCoreProfiles &) = delete;

    bool isValid() const;

    // <data root>/profiles.json — the file this registry owns.
    QString filePath() const;

    QList<ProfileContext> profiles() const;
    // Unknown or invalid id -> invalid ProfileContext.
    ProfileContext profile(const QString &id) const;

    // The ambient persona — per-handle process state mirroring
    // BrowserPaths::activeProfileId; a non-slug set resolves to
    // "default".
    QString activeProfileId() const;
    void setActiveProfileId(const QString &id);

    // Registry row + profiles/<id>/, slugified unique id from the
    // display name — invalid context on failure.
    ProfileContext createProfile(const QString &displayName);
    bool renameProfile(const QString &id, const QString &name);
    // Basename field only — the caller wrote the PNG already.
    bool setAvatarName(const QString &id, const QString &fileName);
    bool removeAvatar(const QString &id);
    // Row + profiles/<id>/ removal; refuses "default" and non-slug
    // ids.  See the class note for the guards that stay on the
    // adoption adapter.
    bool deleteProfile(const QString &id);

    bool reload();

    // Per-profile roots — byte-identical to BrowserPaths' layout; a
    // non-slug id resolves to "default".
    QString profileDir(const QString &id) const;
    QString profileCacheDir(const QString &id) const;
    QString engineStorageDir(const QString &id) const;
    QString engineCacheDir(const QString &id) const;
    QString settingsDir(const QString &id) const;
    // profiles/<id>/<avatar field or profile-avatar.png>.
    QString avatarPath(const QString &id) const;

    // Display name -> filesystem slug ("profile" when nothing
    // survives) — ProfileStore::slugify.
    static QString slugify(const QString &displayName);

private:
    RcProfiles *m_h;
};

#endif // RUSTCOREBRIDGE_H
