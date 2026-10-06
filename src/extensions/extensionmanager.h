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

#include <qlist.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qurl.h>

class QWebEngineProfile;

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

private:
    explicit ExtensionManager(QObject *parent = nullptr);

    void reloadUserScripts(QWebEngineProfile *profile);

    QList<QWebEngineProfile *> m_profiles;
};

#endif // EXTENSIONMANAGER_H
