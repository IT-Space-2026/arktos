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

#include <qjsonobject.h>
#include <qjsonvalue.h>
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
struct AcDlwpolicy;
struct AcPrivacy;
struct AcUrlclean;
struct AcCookies;
struct RcNotes;
struct RcSettings;
struct RcProfiles;

// MB06: the mw envelope registry ids Qt callers dispatch — the wire
// truth is mw.describe (MW-ARCH §7); these mirror the ids it reports
// (append-only, never renumber).  The complete per-domain op tables
// live in rustcore/src/mw/mod.rs.  MB07 added the policy, session,
// icons and opensearch domains; cookies (7) and 9-11 stay flat for
// now — no Qt caller dispatches them.
namespace AcMw {
enum Domain : quint8 {
    Mw = 0,
    Settings = 1,
    Profiles = 2,
    History = 3,
    Bookmarks = 4,
    Policy = 5,
    Session = 6,
    Icons = 8,
    OpenSearch = 12,
    // SYNC03: the serverless sync engine + transport + pairing.
    Sync = 19,
};
enum SettingsOp : quint16 {
    SettingsGet = 0x0001,
    SettingsList = 0x0003,
    SettingsSet = 0x0004,
    SettingsRemove = 0x0005,
    SettingsContains = 0x0006,
    SettingsSync = 0x0007,
    SettingsImportIni = 0x0008,
};
enum ProfilesOp : quint16 {
    ProfilesList = 0x0001,
    ProfilesReload = 0x0002,
    ProfilesGet = 0x0003,
    ProfilesCreate = 0x0004,
    ProfilesRename = 0x0005,
    ProfilesSetAvatar = 0x0006,
    ProfilesDelete = 0x0007,
};
enum HistoryOp : quint16 {
    HistoryCount = 0x0001,
    HistoryAdd = 0x0002,
    HistoryQuery = 0x0003,
    HistorySuggest = 0x0004,
    HistoryExists = 0x0005,
    HistoryUpdateTitle = 0x0006,
    HistoryRemove = 0x0007,
    HistoryClear = 0x0008,
    HistoryClearSince = 0x0009,
    HistoryExpire = 0x000a,
};
enum PolicyOp : quint16 {
    SitedecGet = 0x0001,
    SitedecSet = 0x0002,
    SitedecLookup = 0x0003,
    SitedecRemove = 0x0004,
    SitedecClear = 0x0005,
    SitedecReplace = 0x0006,
    SitedecList = 0x0007,
    SitedecSnapshot = 0x0008,
    SitedecStorePresent = 0x0009,
    SitedecReload = 0x000a,
    SitedecReset = 0x000b,
    PolicyBlocklistReload = 0x000c,
    PolicyTrancoFileEntries = 0x000d,
    PolicyTrancoReload = 0x000e,
    SiteTrustLookup = 0x000f,
};
enum SessionOp : quint16 {
    SessionExists = 0x0001,
    SessionLoad = 0x0002,
    SessionSave = 0x0003,
    SessionClear = 0x0004,
    SessionMarkStart = 0x0005,
    SessionMarkExit = 0x0006,
    SessionMarkTerminated = 0x0007,
};
enum IconsOp : quint16 {
    IconsCount = 0x0001,
    IconsPut = 0x0002,
    IconsGet = 0x0003,
    IconsClear = 0x0004,
    IconsSweep = 0x0005,
};
enum SyncOp : quint16 {
    SyncIdentity = 0x0001,
    SyncMembersList = 0x0002,
    SyncMemberAdd = 0x0003,
    SyncMemberRemove = 0x0004,
    SyncStats = 0x0009,
    SyncTransportStart = 0x000b,
    SyncTransportStop = 0x000c,
    SyncTransportStatus = 0x000d,
    SyncPairBegin = 0x0015,
    SyncPairJoin = 0x0016,
    SyncPairPoll = 0x0017,
    SyncPairConfirm = 0x0018,
    SyncPairCancel = 0x0019,
    SyncPairList = 0x001a,
    // SYNC06: open-tabs presence — publish this device's tab set /
    // read the newest doc each other device published.
    SyncTabsPublish = 0x001b,
    SyncTabsPeers = 0x001c,
    // SYNC09: device management — this profile's paired devices
    // (id, fingerprint, last-seen) and the unpair/revoke command
    // (drops the member, its onion grant, and rotates the
    // profile's group secret).
    SyncDevicesList = 0x0021,
    SyncDeviceRevoke = 0x0022,
    // SYNC11: one bounded exchange round — dial members, ship +
    // pull journal ops, stop the transport if the round started it.
    SyncNow = 0x0024,
};
enum OpenSearchOp : quint16 {
    OseList = 0x0001,
    OsePut = 0x0002,
    OseStorePresent = 0x0003,
    OseGet = 0x0004,
    OseRemove = 0x0005,
    OseRename = 0x0006,
    OseReorder = 0x0007,
    OseSetKeywords = 0x0008,
    OseImport = 0x0009,
    OseEngineForKeyword = 0x000a,
    OseKeywords = 0x000b,
    OseRemovedBundled = 0x000c,
    OseBlockBundled = 0x000d,
    OseUnblockBundled = 0x000e,
    OseBundledNames = 0x000f,
    OseSeedBundled = 0x0010,
    OseRestoreBundled = 0x0011,
    OseExpand = 0x0012,
    OseUrl = 0x0013,
    OseKeywordUrl = 0x0014,
    OseReset = 0x0015,
};
}

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
    // minted against — holders of cached facade handles (m_bm,
    // m_iconStore, the RustCoreSettings/RustCoreProfiles lazy
    // fallbacks) drop and remint on this.
    static bool rootDrifted();

    // RC07: the facade root — NULL only if ac_init itself failed (a
    // warning with ac_last_error() is logged at construction).
    AcCore *core() const { return m_core; }

    // MB06: one envelope round trip through ac_mw_dispatch — the v1
    // frame (20-byte header + JSON payload) in, the op's response
    // payload out.  Both return an invalid (Undefined) QJsonValue on
    // failure; errorOut, when given, receives the response's
    // {"code","status","message"} object, or a synthesized
    // {"code":"transport"} doc on a bare-status failure.  Profile-
    // scoped ops bind through the payload's "profile" field — no
    // handle is minted.  Synchronous on the caller's thread.
    QJsonValue mwQuery(int domain, int op,
                       const QJsonObject &payload = QJsonObject(),
                       QJsonObject *errorOut = nullptr);
    QJsonValue mwCommand(int domain, int op,
                         const QJsonObject &payload = QJsonObject(),
                         QJsonObject *errorOut = nullptr);

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
    // MD04: the download-interception policy handle — ac_dlw_call
    // drives it; release with ac_dlwpolicy_close.
    AcDlwpolicy *dlwpolicy(const QString &profileId = QString());
    // MD05: the per-persona privacy-policy store view — ac_privacy_call
    // and ac_privacy_gpc drive it; release with ac_privacy_close.
    AcPrivacy *privacy(const QString &profileId = QString());
    // MD06: the per-persona URL-sanitize policy — ac_urlclean_call
    // drives it; release with ac_urlclean_close.
    AcUrlclean *urlclean(const QString &profileId = QString());
    // MD23: the per-persona RC15 cookie store — the Chromium lane's
    // durable mirror of the engine jar (CookieJar writes it from the
    // QWebEngineCookieStore signals; chrome.cookies reads it);
    // release with ac_cookies_close.
    AcCookies *cookies(const QString &profileId = QString());
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

// MD08: one granular call into rustcore's sesspolicy module — the
// session snapshot/restore decision layer (debounce + ceiling,
// dirty-on-what, retention caps, crash-restore, startup fallback)
// lives there; the consumers keep their own timers and fallbacks.
// The profile-bound AcSession is minted per call — cheap, and a
// mid-process settings write lands without a stale cached document.
// With no bridge serving, the rc_* spelling runs against a bound
// RcSettings (an empty id binds "default" like every other store
// consumer).  An empty verdict means "run the caller's own
// consult" — an FFI failure can never silently change session
// behavior.
QJsonObject rustCoreSessionPolicyCall(const QString &profileId,
                                      const QJsonObject &request);

// RC02: thin Qt wrapper over the rc_settings_* typed per-profile
// store — the QSettings-shaped surface RC08 swaps in for
// ProfileSettings' persistence.  The scope binds at construction:
// a profile slug opens <data root>/profiles/<id>/settings.json and
// auto-imports the legacy settings.ini on first open; an empty id
// opens the global scope (<data root>/settings.json).
//
// MB06: every op is one envelope round trip through ac_mw_dispatch
// when the bridge can serve — the payload's "profile" field carries
// the scope, so no RcSettings handle is minted at all (the op opens
// the bound store per call inside the crate).  The lazy
// rc_settings_open fallback remains for contexts the bridge cannot
// reach (no app instance yet, a non-GUI first touch, ac_init
// failure).
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
    // MB06: the fallback lane — minted on first use, only when the
    // bridge cannot serve (the envelope path needs no handle).
    RcSettings *fallbackStore() const;
    mutable RcSettings *m_h;
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
// MB06: an empty dataRoot serves the registry ops through the
// envelope (no handle minted); an explicit root keeps the lazy
// rc_profiles_open spelling so test fixtures can bind a
// QTemporaryDir the core never saw, and it is also the fallback
// when the bridge cannot serve.  activeProfileId()/
// setActiveProfileId() stay flat either way — the ambient persona
// is per-handle process state the envelope cannot carry.
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

    // Display name -> filesystem slug ("profile" when nothing
    // survives) — ProfileStore::slugify.
    static QString slugify(const QString &displayName);

private:
    // MB06: same lazy fallback as RustCoreSettings — minted only for
    // the explicit-root spelling or when the bridge cannot serve.
    // The envelope path (m_root empty + serving()) never mints.
    RcProfiles *fallbackStore() const;
    bool servingEnvelope() const;
    mutable RcProfiles *m_h;
    QString m_root;
};

#endif // RUSTCOREBRIDGE_H
