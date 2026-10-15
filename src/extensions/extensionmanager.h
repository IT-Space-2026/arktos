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

#ifndef EXTENSIONMANAGER_H
#define EXTENSIONMANAGER_H

#include <qobject.h>

#include <qhash.h>
#include <qlist.h>
#include <qpointer.h>
#include <qset.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qurl.h>

class QWebEngineExtensionManager;
class QWebEnginePage;
class QWebEngineProfile;
class QNetworkReply;
class QTimer;

// Application-side wrapper around QWebEngineExtensionManager (EXT01).
// The Qt API (QtWebEngine 6.10+, tech preview) loads and installs
// Manifest-V3 Chrome extensions per profile; this class owns the
// per-profile wiring, turns the gated Qt types into plain data for
// the settings UI, inspects manifest.json up front for chrome.* APIs
// Qt WebEngine cannot serve, and runs the user-scripts directory
// through QWebEngineScriptCollection for what extensions cannot cover.
//
// Feature notes (from the Qt documentation):
//   - Manifest V3 only; MV2 packages are rejected outright.
//   - Extensions cannot be loaded on off-the-record profiles.
//     Verified on Qt 6.12.0 (EXT04 probe): loadExtension answers
//     "Can't load in off-the-record mode" and installExtension
//     "Cannot install in off-the-record mode", so installOnProfile()
//     skips OTR profiles for extensions while still attaching the
//     user-scripts collection.  --extension-otr-smoke pins the
//     rejection — if a future Qt lifts it, that smoke fails and the
//     per-extension "allow in private windows" toggle should be
//     built then.
//   - Loaded extensions start disabled; enabling is explicit.
//   - Every profile ships the built-in Google Hangouts and Chromium
//     PDF viewer components; they appear in the list and can be
//     disabled but not uninstalled.
//
// EXT05 trust surface: loadExtension()/installExtension() refuse any
// path that has not passed the consent gate — the review dialog
// records per-path consent on approval (grantConsent()), callers
// with no UI must grant explicitly.  Consent is one-shot: the next
// load/install of the same path needs a fresh review.  The tor
// profile is never wired at all (isInstalledOnProfile() stays false).
//
// SEC25 hardening on top of that base:
//   - Integrity pin: each successful install records a sha256 of the
//     extension's on-disk file tree (QSettings, per profile).  A
//     startup sweep re-hashes and compares — a mismatch flags the
//     extension (ExtensionInfo::tampered, extensionTampered signal)
//     instead of silently loading it.  Installs that predate pinning
//     adopt their current tree as the baseline.
//   - Update drift: applying a downloaded update re-runs the manifest
//     check on the new package and the review dialog lists grants the
//     installed version did not have — original consent never covers
//     escalated permissions.
//   - Broad host access (<all_urls>, "*://*/*", http/https wildcard
//     roots) is visually escalated in the review dialog.
//
// EXT12 honest-degradation surface (per EXT11's measured diagnosis):
// Qt WebEngine serves only a subset of the chrome.* APIs extensions
// expect — chrome.extension, the tabs/webNavigation/scripting/
// notifications/contextMenus/windows/etc. namespaces are compiled in
// but unregistered inside QtWebEngineCore, so no app-side flag can
// enable them.  Rather than letting a doomed extension spin forever
// (Bitwarden's popup literally never finishes bootstrapping), the
// manager measures each enabled background-carrying extension once
// per session — a hidden page on the extension's own origin pings
// the service worker and counts live BACKGROUND contexts — and folds
// that verdict together with the manifest's declared-but-missing
// APIs into ExtensionInfo::health.  The Settings row then reads
// "Limited"/"Not supported" with the reason instead of a naked
// "Enabled", and the action bar turns a click on a dead extension
// into an explanation panel instead of the engine's infinite
// spinner.
class ExtensionManager : public QObject
{
    Q_OBJECT

public:
    // EXT12 degradation verdict — see classifyHealth() for how the
    // manifest gaps and the measured background verdict combine.
    enum class ExtensionHealth {
        Unknown = 0,    // nothing measured yet this session
        Healthy,        // background answered, or nothing missing
        Limited,        // declares chrome.* APIs Qt WebEngine lacks —
                        // partial function expected
        NotSupported    // measured dead — its background service
                        // worker never became ready
    };

    // Result of the once-per-session background-liveness probe.
    // None also covers "no background declared" — nothing to measure.
    enum class ProbeVerdict { Pending = -1, None = 0, Responsive, Dead };

    // Plain-data snapshot of one extension row for the settings page —
    // keeps callers off the QT_CONFIG(webengine_extensions)-gated
    // QWebEngineExtensionInfo so the UI compiles on builds without
    // the feature.
    struct ExtensionInfo {
        QString id;
        QString name;
        QString description;
        QString path;           // unpacked source dir; empty for built-ins
        QString error;          // load/install error text, empty on success
        QUrl actionPopupUrl;    // chrome-extension:// popup, empty if none
        bool loaded = false;
        bool installed = false; // persisted under the profile's installPath
        bool enabled = false;
        bool builtin = false;   // shipped component extension
        bool tampered = false;  // SEC25: on-disk files differ from the
                                // install-time integrity pin
        // EXT12: degradation surface.  health folds the manifest's
        // declared-but-missing chrome.* APIs together with the
        // measured background verdict; healthReason is the
        // user-readable why ("its background service worker never
        // became ready"); missingApis lists the declared chrome.*
        // surfaces Qt WebEngine does not serve.
        ExtensionHealth health = ExtensionHealth::Unknown;
        QString healthReason;
        QStringList missingApis;
        // EXT14 hosted-background runtime tier — "starting"/"hosted"/
        // "failed" while a hidden host exists for this extension,
        // empty otherwise.  hostError carries the last hosted-side
        // failure detail.  When EXT12's ExtensionHealth machinery
        // merges from the main lane, "hosted" is the healthy tier.
        QString hostState;
        QString hostError;
        bool hosted = false;    // host up, chromeBridge handshake done
        // EXT15 shim diagnostics — typed errors the hosted page's
        // gapfill surfaced (APIs the extension relies on that cannot
        // work).  Empty when nothing tripped.
        QStringList shimGaps;
        // EXT15: chrome.action/browserAction setters land here (the
        // hosted runtime has no native action delegate); EXT08's
        // action strip renders badge/title from these on merge.
        QString actionTitle;      // empty = manifest default_title
        QString actionBadgeText;
        QString actionBadgeColor; // css color string, empty = default
        QString actionPopup;      // setPopup override path
        bool actionEnabled = true;
    };

    // Result of reading an unpacked extension's manifest.json — used to
    // warn about MV2 packages and permissions Qt WebEngine cannot
    // satisfy before handing the folder to Chromium.
    struct Manifest {
        bool valid = false;     // manifest.json found and parses
        QString error;          // why invalid / unreadable
        int manifestVersion = 0;
        QString name;
        QString version;
        QString description;
        bool hasBackground = false;
        QStringList backgroundScripts;  // background.scripts then
                                        // background.service_worker,
                                        // in manifest order (EXT14)
        int contentScriptCount = 0;
        bool hasAction = false;
        QStringList permissions;      // permissions + optional_permissions
        QStringList hostPermissions;
        QStringList unsupported;      // permissions Qt WebEngine cannot serve
        QStringList unverified;       // registered but delegates may be stubs
        QStringList dangerous;        // EXT05 deny-by-default consent set —
                                      // by rc_ext_manifest_check under
                                      // CONFIG+=rustcore, or the mirrored
                                      // dangerousPermissions() lists otherwise
        QString packageNote;          // honesty note for packaged installs
                                      // ("unsigned", "structure-only sig")
        QString updateUrl;            // update_url — self-hosted update manifest
    };

    // Result of one extension's update check (EXT03).  Chrome Web
    // Store auto-update does not exist in Qt WebEngine — there is no
    // CRX3 signing verification and no store client — so extensions
    // can only self-update through a manifest "update_url" pointing
    // at a gupdate XML manifest (the static self-hosting update
    // protocol; the Omaha request/POST variant used by
    // update.googleapis.com is not attempted).
    struct UpdateResult {
        QString id;
        QString name;
        QString currentVersion;
        QString error;            // fetch/parse failure detail
        QString availableVersion; // non-empty when the remote is newer
        QUrl codeBase;            // package URL from the update manifest
        QString savedTo;          // downloaded package file, if fetched
        QString packageHash;      // hash_sha256 the update manifest declared
                                  // for the package (hex), empty when absent
        bool noSource = false;    // no update_url — cannot self-update
        bool upToDate = false;    // remote version not newer
        bool installable = false; // a .zip savedTo that installExtension()
                                  // can apply once consent is granted —
                                  // update checks NEVER install on their
                                  // own (EXT05: apply is opt-in)
    };

    static ExtensionManager *instance();

    // False when Qt was built without webengine_extensions — every
    // other call then degrades to an empty list / errorOccurred signal.
    static bool isSupported();

    // Attaches the profile's QWebEngineExtensionManager and injects the
    // user-scripts directory.  Off-the-record profiles are skipped for
    // extension loading — the engine refuses both load and install on
    // them (verified Qt 6.12.0) — but still get user scripts, matching
    // userStyleSheet behavior.
    void installOnProfile(QWebEngineProfile *profile);

    // True once installOnProfile() has run for the profile — the tor
    // profile must never appear here (extensions are a
    // deanonymization surface in that window).
    bool isInstalledOnProfile(QWebEngineProfile *profile) const;

    QList<ExtensionInfo> extensions() const;
    QString installPath() const;

    // Consent gate (EXT05): grantConsent() records one approval for a
    // path — ExtensionReviewDialog::review() grants on accept, so the
    // UI needs no extra call; headless callers (smokes, tests) grant
    // explicitly to stand in for the user.  load/install refuse a
    // path with no recorded consent via errorOccurred — a silent
    // extension install is a bug.  Each grant is consumed by the
    // load/install it authorizes.
    void grantConsent(const QString &path);
    bool hasConsent(const QString &path) const;

    void loadExtension(const QString &path);     // unpacked dir, this session only
    void installExtension(const QString &path);  // dir or .zip, persists in profile
    void removeExtension(const QString &id);     // unload or uninstall, per state
    void setExtensionEnabled(const QString &id, bool enabled);

    // EXT07 default-installed bundle: every *.zip package (and every
    // unpacked subdir carrying manifest.json) in
    // bundledExtensionsPath() is installed onto each prepared
    // non-off-the-record profile exactly once.  The per-profile
    // QSettings list "extensions/bundled/<storageName>" records the
    // basenames already consumed — a user uninstall is never
    // resurrected, and a same-named extension already present
    // (manual install or an older bundle) counts as satisfied.
    // On an extension-fresh profile (nothing in installPath yet)
    // the bundle is enabled as part of the install; profiles that
    // already carry extensions get it installed-but-disabled —
    // opting in is a Preferences checkbox away, and an existing
    // install is never force-enabled retroactively.  Consent is
    // programmatic on this internal path (bundling IS Arora's
    // approval) but still flows through the same one-shot
    // grant/consume accounting, so a later manual install of the
    // same path needs a real review like any other.
    void installBundledExtensions(QWebEngineProfile *profile);
    // Directory scanned by installBundledExtensions():
    // ARORA_BUNDLED_EXTENSIONS_DIR wins (tests/dev overrides), then
    // <installedDataDirectory>/extensions, then the in-tree
    // data/extensions for uninstalled builds (the binary links into
    // src/ or the repository root).  Empty when none exists.
    static QString bundledExtensionsPath();

    // Reads <extensionDir>/manifest.json; path may point at the
    // manifest file itself.  valid==false for .zip packages (not
    // inspected — Qt's installer handles them).
    static Manifest inspectManifest(const QString &path);

    // EXT03 update checks.  checkForUpdates() GETs every extension's
    // update_url through the application-side network manager,
    // compares dotted-quad versions and downloads the package into the
    // updates directory — https only (loopback http is allowed for
    // local fixtures).  EXT05: a check never installs anything; a
    // downloaded .zip is flagged installable and the UI offers it
    // through the same consent review as a manual install (packages
    // carrying the manifest "key" update the extension in place under
    // the same id).  Anything else — typically a .crx Qt cannot
    // install — is saved for manual install and flagged in the
    // result.
    void checkForUpdates(bool manual = false);
    bool updateCheckInProgress() const;
    UpdateResult updateResultFor(const QString &id) const;
    QString updateUrlFor(const QString &id) const;

    // Directory the downloaded update packages land in.
    static QString updatesPath();
    // <0 / 0 / >0 like strcmp, over dotted numeric components —
    // "1.0" == "1.0.0", "1.10" > "1.9".  Components past the fourth
    // and non-numeric input compare lexically.
    static int compareVersions(const QString &a, const QString &b);
    // Parses a gupdate XML response into the result's
    // availableVersion/codeBase (status ok) or upToDate
    // (noupdate/not-newer), setting error otherwise.  expectedId
    // matches <app appid> when the document carries more than one
    // extension; a single-app document is accepted regardless — the
    // Chromium-derived id is opaque to self-hosted manifests.
    static bool parseUpdateManifest(const QByteArray &xml,
                                    const QString &expectedId,
                                    UpdateResult *result);

    // Opt-in periodic check ("extensions/autoUpdateCheck", default
    // off — silent code updates are a supply-chain risk, so the
    // background check only detects and downloads; installs still
    // come from the manual button).  While enabled, checks run at
    // most once every 20 hours.
    static bool updateCheckEnabled();
    static void setUpdateCheckEnabled(bool enabled);

    // chrome.* permission names whose browser-side delegates Qt
    // WebEngine does not provide — see extensionmanager.cpp for the
    // rationale list.
    static QStringList unsupportedPermissions();
    // Registered APIs whose Qt delegates are only partially plumbed;
    // functionality may work but is not guaranteed.
    static QStringList unverifiedPermissions();
    // EXT05 deny-by-default consent set — permissions and host
    // patterns that require an explicit per-item consent checkbox in
    // the review dialog (install is refused otherwise).  Mirrors
    // DANGEROUS_PERMISSIONS/DANGEROUS_HOST_PATTERNS in rustcore's
    // extverify.rs — keep the lists in sync.
    static QStringList dangerousPermissions();
    static QStringList dangerousHostPatterns();

    // SEC25: broad host access — "<all_urls>" or a whole-scheme host
    // wildcard ("*://*/*", "http://*/*", "https://*/*", "file:///*"
    // …): the extension can read and change data on every site the
    // scheme covers.  The review dialog escalates these visually;
    // subdomain wildcards like "*://*.example.com/*" are not broad.
    static bool isBroadHostPattern(const QString &pattern);

    // EXT12: fold the two degradation signals into one row state.
    // A measured-dead background beats everything — the extension is
    // NotSupported (its reason names the missing chrome.* surfaces
    // when the manifest declares any).  Declared-but-missing APIs on
    // a live (or unprobed) extension mean Limited.  ProbeVerdict is
    // what the last background probe recorded; Pending/-1 means none
    // has run for the id this session.
    static ExtensionHealth classifyHealth(bool hasBackground,
            const QStringList &missingApis, ProbeVerdict verdict,
            QString *reason = nullptr);

    // EXT12: queue a background-liveness probe for one extension —
    // smokes and tests drive this directly; in normal operation the
    // changed()-triggered scan queues it.  force re-measures even
    // when a verdict already exists.
    void requestHealthProbe(const QString &id, bool force);

    // The last probe's machine-readable detail (e.g. "ping=TIMEOUT",
    // "ping=PONG backgrounds=1") — diagnostics for smokes/tests and
    // the Preferences details pane; empty until a probe finishes.
    QString healthProbeDetail(const QString &id) const;

    // SEC25 install-integrity pin: hex sha256 over an unpacked
    // extension's whole file tree — every file's relative path and
    // contents, hashed in sorted order.  Empty when the directory
    // cannot be enumerated.
    static QByteArray extensionTreeHash(const QString &dirPath);
    // Re-runs the startup integrity sweep for one profile: every
    // installed extension's on-disk tree is hashed and compared
    // against the pin recorded when it installed.  A mismatch flags
    // the extension (ExtensionInfo::tampered + extensionTampered)
    // and stays loaded — "files changed since install" is the user's
    // decision to review or remove, not a silent block.  Installs
    // that predate pinning adopt their current tree as the baseline.
    void verifyInstalledIntegrity(QWebEngineProfile *profile);

    // User scripts: every *.js file in this directory is injected at
    // DocumentReady in the ApplicationWorld on each prepared profile.
    static QString userScriptsPath();
    QStringList userScriptNames() const;
    void reloadUserScripts();

signals:
    void changed();                                   // extension list updated
    void extensionLoaded(const ExtensionManager::ExtensionInfo &info);
    void extensionInstalled(const ExtensionManager::ExtensionInfo &info);
    void extensionUnloaded(const ExtensionManager::ExtensionInfo &info);
    void extensionUninstalled(const ExtensionManager::ExtensionInfo &info);
    void extensionTampered(const ExtensionManager::ExtensionInfo &info); // SEC25
    void errorOccurred(const QString &message);       // unsupported build, bad call
    void userScriptsChanged();

    void updateCheckStarted();
    void updateCheckFinished(const QList<ExtensionManager::UpdateResult> &results);
    void updateAvailable(const ExtensionManager::UpdateResult &result);

private:
    explicit ExtensionManager(QObject *parent = nullptr);

    // EXT12 health probe plumbing — see extensionmanager.cpp.  One
    // hidden extension-origin page at a time; probes queue behind the
    // canonical (non-OTR) profile's extension manager.
    void enqueuePendingProbes();
    void startNextHealthProbe();
    void runHealthProbeScript();
    void finishHealthProbe(ProbeVerdict verdict, const QString &detail);
    void evaluateHealthReport(const QByteArray &json);
    void abandonHealthProbe();

    void finishBundledInstall(const ExtensionInfo &info);
    void scheduleIntegritySweep(QWebEngineProfile *profile, int attempt);
    void pinInstalledExtension(QWebEngineProfile *profile,
                               const ExtensionInfo &info);
    static QString integritySettingsKey(const QString &storageName,
                                        const QString &id);
    void reloadUserScripts(QWebEngineProfile *profile);
    void scheduleAutoUpdateCheck();
    void maybeAutoUpdateCheck();
    void fetchUpdateManifest(const UpdateResult &result,
                             const QUrl &updateUrl);
    void fetchUpdatePackage(const UpdateResult &result);
    void finishUpdateCheck();

    QList<QPointer<QWebEngineProfile>> m_profiles;

    // One-shot consent grants keyed by the exact path string the
    // review approved; consumed by the load/install they authorize.
    QSet<QString> m_consentedPaths;

    // SEC25: extension ids whose installed tree failed the integrity
    // sweep this session; cleared on uninstall and re-pin.
    QSet<QString> m_tamperedIds;

    // EXT07: bundled installs in flight, correlated to their
    // installFinished by the package-name stem so the per-profile
    // consumed marker and the fresh-profile enable land on the right
    // extension.  Entries for installs that never finish simply leak
    // out of the list (bounded — a handful of bundled files).
    struct PendingBundle {
        QPointer<QWebEngineExtensionManager> engine;
        QString settingsKey;    // "extensions/bundled/<storageName>"
        QString fileName;       // basename — the consumed marker
        QString stem;           // name stem for loose correlation
        bool enable = false;    // extension-fresh profile → enable
    };
    QList<PendingBundle> m_bundledPending;

    // In-flight update jobs keyed by the reply currently active for
    // the extension (manifest fetch, then package download).
    struct UpdateJob {
        UpdateResult result;
        QUrl updateUrl;
    };
    QHash<QNetworkReply *, UpdateJob> m_updateJobs;
    QList<UpdateResult> m_updateResults;
    QHash<QString, UpdateResult> m_lastUpdateResults;
    QHash<QString, QString> m_updateUrls;
    bool m_updateCheckRunning = false;
    bool m_autoUpdateScheduled = false;

    // EXT12: once-per-session health verdicts + the probe queue.
    // m_healthVerdicts is keyed by extension id and filled as probes
    // land; an absent id was never probed this session (enqueued ids
    // get ProbeVerdict::Pending so they are not double-queued).
    QHash<QString, ProbeVerdict> m_healthVerdicts;
    QHash<QString, QString> m_healthDetails;
    QList<QPair<QString, QPointer<QWebEngineProfile>>> m_healthQueue;
    QPointer<QWebEnginePage> m_healthPage;
    QPointer<QWebEngineProfile> m_healthProfile;
    QString m_healthProbeId;
    bool m_healthPopupStage = false;
    bool m_healthScanScheduled = false;
};

#endif // EXTENSIONMANAGER_H
