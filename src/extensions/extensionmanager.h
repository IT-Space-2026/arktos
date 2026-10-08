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
#include <qstring.h>
#include <qstringlist.h>
#include <qurl.h>

class QWebEngineProfile;
class QNetworkReply;

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
//   - Extensions cannot be loaded on off-the-record profiles, so
//     installOnProfile() skips them.
//   - Loaded extensions start disabled; enabling is explicit.
//   - Every profile ships the built-in Google Hangouts and Chromium
//     PDF viewer components; they appear in the list and can be
//     disabled but not uninstalled.
class ExtensionManager : public QObject
{
    Q_OBJECT

public:
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
        int contentScriptCount = 0;
        bool hasAction = false;
        QStringList permissions;      // permissions + optional_permissions
        QStringList hostPermissions;
        QStringList unsupported;      // permissions Qt WebEngine cannot serve
        QStringList unverified;       // registered but delegates may be stubs
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
        bool noSource = false;    // no update_url — cannot self-update
        bool upToDate = false;    // remote version not newer
        bool installTriggered = false; // .zip handed to installExtension()
    };

    static ExtensionManager *instance();

    // False when Qt was built without webengine_extensions — every
    // other call then degrades to an empty list / errorOccurred signal.
    static bool isSupported();

    // Attaches the profile's QWebEngineExtensionManager and injects the
    // user-scripts directory.  Off-the-record profiles are skipped for
    // extension loading (unsupported upstream) but still get user
    // scripts, matching userStyleSheet behavior.
    void installOnProfile(QWebEngineProfile *profile);

    QList<ExtensionInfo> extensions() const;
    QString installPath() const;

    void loadExtension(const QString &path);     // unpacked dir, this session only
    void installExtension(const QString &path);  // dir or .zip, persists in profile
    void removeExtension(const QString &id);     // unload or uninstall, per state
    void setExtensionEnabled(const QString &id, bool enabled);

    // Reads <extensionDir>/manifest.json; path may point at the
    // manifest file itself.  valid==false for .zip packages (not
    // inspected — Qt's installer handles them).
    static Manifest inspectManifest(const QString &path);

    // EXT03 update checks.  checkForUpdates() GETs every extension's
    // update_url through the application-side network manager,
    // compares dotted-quad versions and downloads the package into the
    // updates directory.  A .zip package is handed to installExtension
    // (packages carrying the manifest "key" update the extension in
    // place under the same id); anything else — typically a .crx Qt
    // cannot install — is saved for manual install and flagged in the
    // result.  Automatic checks only detect + download; installing is
    // reserved for the user-initiated check.
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
    void errorOccurred(const QString &message);       // unsupported build, bad call
    void userScriptsChanged();

    void updateCheckStarted();
    void updateCheckFinished(const QList<ExtensionManager::UpdateResult> &results);
    void updateAvailable(const ExtensionManager::UpdateResult &result);

private:
    explicit ExtensionManager(QObject *parent = nullptr);

    void reloadUserScripts(QWebEngineProfile *profile);
    void scheduleAutoUpdateCheck();
    void maybeAutoUpdateCheck();
    void fetchUpdateManifest(const UpdateResult &result,
                             const QUrl &updateUrl);
    void fetchUpdatePackage(const UpdateResult &result);
    void finishUpdateCheck();

    QList<QWebEngineProfile *> m_profiles;

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
    bool m_updateCheckManual = false;
    bool m_autoUpdateScheduled = false;
};

#endif // EXTENSIONMANAGER_H
