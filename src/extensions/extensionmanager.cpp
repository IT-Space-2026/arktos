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

#include "extensionmanager.h"

#include "browserpaths.h"

#include <qcoreapplication.h>
#include <qdebug.h>
#include <qdir.h>
#include <qfile.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qtimer.h>

#include <qwebengineprofile.h>
#include <qwebenginescript.h>
#include <qwebenginescriptcollection.h>

#include <qwebengineextensionmanager.h>

#if QT_CONFIG(webengine_extensions)

// chromium-extension IDs of the component extensions every profile
// ships (also detectable by their empty install path).
static const char *const kBuiltinExtensionIds[] = {
    "mhjfbmdgcfjbbpaeojofohoefgiehjai", // Chromium PDF viewer
    "nkeimhogjdpnpccoofpliimaahmaaome", // Google Hangouts
};

static ExtensionManager::ExtensionInfo infoFrom(const QWebEngineExtensionInfo &info)
{
    ExtensionManager::ExtensionInfo out;
    out.id = info.id();
    out.name = info.name();
    out.description = info.description();
    out.path = info.path();
    out.error = info.error();
    out.actionPopupUrl = info.actionPopupUrl();
    out.loaded = info.isLoaded();
    out.installed = info.isInstalled();
    out.enabled = info.isEnabled();
    out.builtin = out.path.isEmpty();
    for (const char *id : kBuiltinExtensionIds) {
        if (out.id == QLatin1String(id))
            out.builtin = true;
    }
    return out;
}

// The canonical UI-facing manager: the first prepared non-off-the-
// record profile's manager.  prepareProfile() hands out profiles in
// order, so this is the "arora" browsing profile.
static QWebEngineExtensionManager *managerFor(const QList<QWebEngineProfile *> &profiles)
{
    for (QWebEngineProfile *profile : profiles) {
        if (profile && !profile->isOffTheRecord() && profile->extensionManager())
            return profile->extensionManager();
    }
    return 0;
}

#endif // QT_CONFIG(webengine_extensions)

ExtensionManager::ExtensionManager(QObject *parent)
    : QObject(parent)
{
}

ExtensionManager *ExtensionManager::instance()
{
    static ExtensionManager *manager = 0;
    if (!manager)
        manager = new ExtensionManager(qApp);
    return manager;
}

bool ExtensionManager::isSupported()
{
#if QT_CONFIG(webengine_extensions)
    return true;
#else
    return false;
#endif
}

void ExtensionManager::installOnProfile(QWebEngineProfile *profile)
{
    if (!profile || m_profiles.contains(profile))
        return;
    m_profiles.append(profile);

    // User scripts are the app's own injection — they apply to
    // off-the-record pages too, like the user style sheet.
    reloadUserScripts(profile);

#if QT_CONFIG(webengine_extensions)
    // Qt WebEngine cannot load extensions into off-the-record
    // profiles; the OTR profile carries only its built-in components.
    if (profile->isOffTheRecord())
        return;

    QWebEngineExtensionManager *extensions = profile->extensionManager();
    if (!extensions)
        return;

    const auto relay = [this](void (ExtensionManager::*signal)(const ExtensionInfo &)) {
        return [this, signal](const QWebEngineExtensionInfo &info) {
            emit (this->*signal)(infoFrom(info));
            emit changed();
        };
    };
    connect(extensions, &QWebEngineExtensionManager::loadFinished,
            this, relay(&ExtensionManager::extensionLoaded));
    connect(extensions, &QWebEngineExtensionManager::installFinished,
            this, [this](const QWebEngineExtensionInfo &info) {
        emit extensionInstalled(infoFrom(info));
        emit changed();
    });
    connect(extensions, &QWebEngineExtensionManager::unloadFinished,
            this, relay(&ExtensionManager::extensionUnloaded));
    connect(extensions, &QWebEngineExtensionManager::uninstallFinished,
            this, relay(&ExtensionManager::extensionUninstalled));
#endif
}

QList<ExtensionManager::ExtensionInfo> ExtensionManager::extensions() const
{
    QList<ExtensionInfo> list;
#if QT_CONFIG(webengine_extensions)
    QWebEngineExtensionManager *extensions = managerFor(m_profiles);
    if (!extensions)
        return list;
    const QList<QWebEngineExtensionInfo> loaded = extensions->extensions();
    for (const QWebEngineExtensionInfo &info : loaded)
        list.append(infoFrom(info));
#endif
    return list;
}

QString ExtensionManager::installPath() const
{
#if QT_CONFIG(webengine_extensions)
    if (QWebEngineExtensionManager *extensions = managerFor(m_profiles))
        return extensions->installPath();
#endif
    return QString();
}

void ExtensionManager::loadExtension(const QString &path)
{
#if QT_CONFIG(webengine_extensions)
    if (QWebEngineExtensionManager *extensions = managerFor(m_profiles)) {
        extensions->loadExtension(path);
        return;
    }
#endif
    emit errorOccurred(tr("Extension support is not available in this build of Qt WebEngine."));
}

void ExtensionManager::installExtension(const QString &path)
{
#if QT_CONFIG(webengine_extensions)
    if (QWebEngineExtensionManager *extensions = managerFor(m_profiles)) {
        // Accepts an unpacked directory or a packaged .zip; installed
        // extensions persist under the profile's installPath().
        extensions->installExtension(path);
        return;
    }
#endif
    emit errorOccurred(tr("Extension support is not available in this build of Qt WebEngine."));
}

void ExtensionManager::removeExtension(const QString &id)
{
#if QT_CONFIG(webengine_extensions)
    if (QWebEngineExtensionManager *extensions = managerFor(m_profiles)) {
        const QList<QWebEngineExtensionInfo> loaded = extensions->extensions();
        for (const QWebEngineExtensionInfo &info : loaded) {
            if (info.id() != id)
                continue;
            if (info.isInstalled())
                extensions->uninstallExtension(info);
            else
                extensions->unloadExtension(info);
            return;
        }
        emit errorOccurred(tr("Extension %1 is no longer loaded.").arg(id));
        return;
    }
#endif
    emit errorOccurred(tr("Extension support is not available in this build of Qt WebEngine."));
}

void ExtensionManager::setExtensionEnabled(const QString &id, bool enabled)
{
#if QT_CONFIG(webengine_extensions)
    if (QWebEngineExtensionManager *extensions = managerFor(m_profiles)) {
        const QList<QWebEngineExtensionInfo> loaded = extensions->extensions();
        for (const QWebEngineExtensionInfo &info : loaded) {
            if (info.id() != id)
                continue;
            extensions->setExtensionEnabled(info, enabled);
            // Qt exposes no enabledChanged signal — refresh the UI
            // once Chromium has had a moment to apply the toggle.
            QTimer::singleShot(400, this, [this]() {
                emit changed();
            });
            return;
        }
        emit errorOccurred(tr("Extension %1 is no longer loaded.").arg(id));
        return;
    }
#endif
    emit errorOccurred(tr("Extension support is not available in this build of Qt WebEngine."));
}

ExtensionManager::Manifest ExtensionManager::inspectManifest(const QString &path)
{
    Manifest manifest;
    QFileInfo info(path);
    QString manifestPath;
    if (info.isDir())
        manifestPath = info.absoluteFilePath() + QLatin1String("/manifest.json");
    else if (info.isFile() && info.fileName() == QLatin1String("manifest.json"))
        manifestPath = info.absoluteFilePath();
    else if (info.isFile() && info.suffix() == QLatin1String("zip")) {
        // Qt's installer accepts .zip packages; the manifest inside is
        // not inspected here — Chromium reports errors through
        // installFinished's error string instead.
        manifest.error = tr("zip package — manifest not inspected before install");
        return manifest;
    } else {
        manifest.error = tr("not an extension folder or manifest.json");
        return manifest;
    }

    QFile file(manifestPath);
    if (!file.open(QIODevice::ReadOnly)) {
        manifest.error = tr("cannot read %1").arg(manifestPath);
        return manifest;
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        manifest.error = tr("manifest.json is not valid JSON: %1")
            .arg(parseError.errorString());
        return manifest;
    }

    const QJsonObject root = doc.object();
    manifest.valid = true;
    manifest.manifestVersion = root.value(QLatin1String("manifest_version")).toInt();
    manifest.name = root.value(QLatin1String("name")).toString();
    manifest.version = root.value(QLatin1String("version")).toString();
    manifest.description = root.value(QLatin1String("description")).toString();
    manifest.hasBackground = root.contains(QLatin1String("background"));
    manifest.hasAction = root.contains(QLatin1String("action"))
        || root.contains(QLatin1String("browser_action"))
        || root.contains(QLatin1String("page_action"));
    manifest.contentScriptCount =
        root.value(QLatin1String("content_scripts")).toArray().size();

    const auto stringList = [&root](const QString &key) {
        QStringList list;
        const QJsonArray entries = root.value(key).toArray();
        for (const QJsonValue &value : entries)
            list.append(value.toString());
        return list;
    };
    manifest.permissions = stringList(QLatin1String("permissions"))
        + stringList(QLatin1String("optional_permissions"));
    manifest.hostPermissions = stringList(QLatin1String("host_permissions"));

    const QStringList unsupported = unsupportedPermissions();
    const QStringList unverified = unverifiedPermissions();
    for (const QString &permission : manifest.permissions) {
        if (unsupported.contains(permission))
            manifest.unsupported.append(permission);
        else if (unverified.contains(permission))
            manifest.unverified.append(permission);
    }

    if (manifest.manifestVersion != 3)
        manifest.error = tr("Qt WebEngine supports Manifest V3 extensions only "
                            "(this package declares version %1).")
            .arg(manifest.manifestVersion);
    return manifest;
}

QStringList ExtensionManager::unsupportedPermissions()
{
    // chrome.* APIs whose browser-side delegates do not exist in Qt
    // WebEngine's extension preview (Qt ships delegates only for
    // runtime + management; Chrome-generated APIs are registered but
    // have no backing implementation).  An extension declaring one of
    // these will load but the API calls fail.
    static const QStringList unsupported = {
        QStringLiteral("accessibilityFeatures.modify"),
        QStringLiteral("accessibilityFeatures.read"),
        QStringLiteral("audio"),
        QStringLiteral("bookmarks"),
        QStringLiteral("browsingData"),
        QStringLiteral("certificateProvider"),
        QStringLiteral("contentSettings"),
        QStringLiteral("contextMenus"),
        QStringLiteral("cookies"),
        QStringLiteral("debugger"),
        QStringLiteral("desktopCapture"),
        QStringLiteral("documentScan"),
        QStringLiteral("downloads"),
        QStringLiteral("enterprise.deviceAttributes"),
        QStringLiteral("enterprise.hardwarePlatform"),
        QStringLiteral("enterprise.platformKeys"),
        QStringLiteral("fileBrowserHandler"),
        QStringLiteral("fileSystemProvider"),
        QStringLiteral("fontSettings"),
        QStringLiteral("gcm"),
        QStringLiteral("history"),
        QStringLiteral("identity"),
        QStringLiteral("idle"),
        QStringLiteral("input.ime"),
        QStringLiteral("loginState"),
        QStringLiteral("menus"),
        QStringLiteral("nativeMessaging"),
        QStringLiteral("networking.config"),
        QStringLiteral("notifications"),
        QStringLiteral("omnibox"),
        QStringLiteral("pageCapture"),
        QStringLiteral("platformKeys"),
        QStringLiteral("power"),
        QStringLiteral("printerProvider"),
        QStringLiteral("privacy"),
        QStringLiteral("processes"),
        QStringLiteral("proxy"),
        QStringLiteral("readingList"),
        QStringLiteral("sessions"),
        QStringLiteral("sidePanel"),
        QStringLiteral("system.cpu"),
        QStringLiteral("system.display"),
        QStringLiteral("system.memory"),
        QStringLiteral("system.storage"),
        QStringLiteral("tabCapture"),
        QStringLiteral("tabGroups"),
        QStringLiteral("tabs"),
        QStringLiteral("topSites"),
        QStringLiteral("tts"),
        QStringLiteral("ttsEngine"),
        QStringLiteral("vpnProvider"),
        QStringLiteral("wallpaper"),
        QStringLiteral("webAuthenticationProxy"),
        QStringLiteral("windows"),
    };
    return unsupported;
}

QStringList ExtensionManager::unverifiedPermissions()
{
    // APIs that are registered in Qt WebEngine's extension build and
    // may partially work — the browser-side plumbing is either
    // extension-internal or only stubbed, so behavior is not
    // guaranteed.
    static const QStringList unverified = {
        QStringLiteral("declarativeNetRequest"),
        QStringLiteral("declarativeNetRequestWithHostAccess"),
        QStringLiteral("declarativeNetRequestFeedback"),
        QStringLiteral("offscreen"),
        QStringLiteral("scripting"),
        QStringLiteral("webNavigation"),
        QStringLiteral("webRequest"),
    };
    return unverified;
}

QString ExtensionManager::userScriptsPath()
{
    const QString directory =
        BrowserPaths::dataFilePath(QLatin1String("userscripts"));
    QDir().mkpath(directory);
    return directory;
}

QStringList ExtensionManager::userScriptNames() const
{
    return QDir(userScriptsPath(), QLatin1String("*.js"),
              QDir::Name, QDir::Files).entryList();
}

void ExtensionManager::reloadUserScripts()
{
    for (QWebEngineProfile *profile : m_profiles) {
        if (profile)
            reloadUserScripts(profile);
    }
    emit userScriptsChanged();
}

void ExtensionManager::reloadUserScripts(QWebEngineProfile *profile)
{
    static const QString prefix = QStringLiteral("userscript:");
    QWebEngineScriptCollection *scripts = profile->scripts();

    const QList<QWebEngineScript> installed = scripts->toList();
    for (const QWebEngineScript &script : installed) {
        if (script.name().startsWith(prefix))
            scripts->remove(script);
    }

    const QString directory = userScriptsPath();
    const QDir dir(directory, QLatin1String("*.js"), QDir::Name, QDir::Files);
    const QStringList files = dir.entryList();
    for (const QString &fileName : files) {
        QFile file(dir.absoluteFilePath(fileName));
        if (!file.open(QIODevice::ReadOnly))
            continue;
        QWebEngineScript script;
        script.setName(prefix + fileName);
        script.setInjectionPoint(QWebEngineScript::DocumentReady);
        script.setRunsOnSubFrames(true);
        script.setWorldId(QWebEngineScript::ApplicationWorld);
        script.setSourceCode(QString::fromUtf8(file.readAll()));
        scripts->insert(script);
    }
}
