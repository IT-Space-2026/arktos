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
#include "networkaccessmanager.h"

#include <qcoreapplication.h>
#include <qdatetime.h>
#include <qdebug.h>
#include <qdir.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qnetworkreply.h>
#include <qnetworkrequest.h>
#include <qregularexpression.h>
#include <qsavefile.h>
#include <qsettings.h>
#include <qtimer.h>
#include <qxmlstream.h>

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
    return nullptr;
}

#endif // QT_CONFIG(webengine_extensions)

ExtensionManager::ExtensionManager(QObject *parent)
    : QObject(parent)
{
}

ExtensionManager *ExtensionManager::instance()
{
    static ExtensionManager *manager = nullptr;
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
    // Qt WebEngine refuses extensions on off-the-record profiles —
    // loadFinished answers "Can't load in off-the-record mode" and
    // installFinished "Cannot install in off-the-record mode" (EXT04,
    // verified Qt 6.12.0).  The OTR profile carries only its built-in
    // components plus the user scripts injected above.
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

    scheduleAutoUpdateCheck();
#endif
}

bool ExtensionManager::isInstalledOnProfile(QWebEngineProfile *profile) const
{
    return m_profiles.contains(profile);
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
    // manifests are kilobytes; refuse absurd sizes before parsing.
    if (file.size() > 1024 * 1024 || !file.open(QIODevice::ReadOnly)) {
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
    manifest.updateUrl = root.value(QLatin1String("update_url")).toString();

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

// -- EXT03: update_url self-update checks -------------------------------
//
// Chrome Web Store auto-update does not exist in Qt WebEngine (no
// CRX3 verification, no store client).  What is supported: the
// self-hosting update protocol — the manifest's update_url points at
// a gupdate XML document.  We GET that document (the dynamic Omaha
// POST request is not attempted — static update manifests are the
// common self-hosting shape and answer plain GETs), compare the
// dotted-quad version and download the declared package.  Zip
// packages install through the Qt extension manager — a package that
// carries the manifest "key" lands under the same extension id and
// replaces the old version; a package without it installs as a
// separate extension, so installs are limited to the user-initiated
// check.  .crx packages cannot be installed by Qt at all — they are
// saved under updatesPath() for manual handling.

namespace {
// A real reply buffers whatever the peer sends; abort past the cap
// rather than letting a hostile endpoint grow memory unbounded.
constexpr qint64 kUpdateManifestMaxBytes = 1024 * 1024;
constexpr qint64 kUpdatePackageMaxBytes = 64 * 1024 * 1024;
constexpr int kUpdateFetchTimeoutMs = 20000;
constexpr int kUpdateDownloadTimeoutMs = 60000;
}

QString ExtensionManager::updatesPath()
{
    const QString directory =
        BrowserPaths::dataFilePath(QLatin1String("extension-updates"));
    QDir().mkpath(directory);
    return directory;
}

int ExtensionManager::compareVersions(const QString &a, const QString &b)
{
    const QStringList partsA = a.split(QLatin1Char('.'));
    const QStringList partsB = b.split(QLatin1Char('.'));
    const int count = qMax(partsA.size(), partsB.size());
    for (int i = 0; i < count; ++i) {
        // Chrome version semantics: a missing component is 0, so
        // "1.0" and "1.0.0" compare equal.
        QString pa = i < partsA.size() ? partsA.at(i) : QString();
        QString pb = i < partsB.size() ? partsB.at(i) : QString();
        if (pa.isEmpty())
            pa = QStringLiteral("0");
        if (pb.isEmpty())
            pb = QStringLiteral("0");
        bool okA = false, okB = false;
        const unsigned long va = pa.toULong(&okA);
        const unsigned long vb = pb.toULong(&okB);
        if (okA && okB) {
            if (va != vb)
                return va < vb ? -1 : 1;
            continue;
        }
        const int cmp = QString::compare(pa, pb);
        if (cmp != 0)
            return cmp < 0 ? -1 : 1;
    }
    return 0;
}

bool ExtensionManager::parseUpdateManifest(const QByteArray &xml,
                                           const QString &expectedId,
                                           UpdateResult *result)
{
    struct Offer {
        QString appId;
        QString status;
        QString codebase;
        QString version;
    };
    QList<Offer> offers;
    QString currentApp;
    QXmlStreamReader reader(xml);
    while (!reader.atEnd()) {
        const QXmlStreamReader::TokenType token = reader.readNext();
        if (token == QXmlStreamReader::StartElement) {
            if (reader.name() == QLatin1String("app")) {
                currentApp = reader.attributes()
                    .value(QLatin1String("appid")).toString();
            } else if (reader.name() == QLatin1String("updatecheck")
                       && !currentApp.isNull()) {
                const QXmlStreamAttributes attrs = reader.attributes();
                Offer offer;
                offer.appId = currentApp;
                offer.status = attrs.value(QLatin1String("status")).toString();
                offer.codebase = attrs.value(QLatin1String("codebase")).toString();
                offer.version = attrs.value(QLatin1String("version")).toString();
                offers.append(offer);
            }
        } else if (token == QXmlStreamReader::EndElement
                   && reader.name() == QLatin1String("app")) {
            currentApp = QString();
        }
    }
    if (reader.hasError()) {
        result->error = tr("update manifest is not valid XML: %1")
            .arg(reader.errorString());
        return false;
    }
    if (offers.isEmpty()) {
        result->error = tr("update manifest carries no updatecheck entry");
        return false;
    }

    // Multi-app documents must name us; single-app documents are
    // accepted regardless of appid — self-hosted manifests are static
    // and the extension id is opaque to the author.
    const Offer *offer = nullptr;
    if (offers.size() == 1) {
        offer = &offers.constFirst();
    } else {
        for (const Offer &candidate : offers) {
            if (candidate.appId == expectedId) {
                offer = &candidate;
                break;
            }
        }
    }
    if (!offer) {
        result->error = tr("update manifest has no entry for this extension");
        return false;
    }
    if (!offer->status.isEmpty()
        && offer->status != QLatin1String("ok")) {
        if (offer->status == QLatin1String("noupdate"))
            result->upToDate = true;
        else
            result->error = tr("update server reported status \"%1\"")
                .arg(offer->status);
        return true;
    }
    if (offer->version.isEmpty()
        || compareVersions(offer->version, result->currentVersion) <= 0) {
        result->upToDate = true;
        return true;
    }
    const QUrl codebase = QUrl::fromUserInput(offer->codebase);
    if (codebase.scheme() != QLatin1String("https")
        && codebase.scheme() != QLatin1String("http")) {
        result->error = tr("update package URL \"%1\" is not http(s)")
            .arg(offer->codebase);
        return false;
    }
    result->availableVersion = offer->version;
    result->codeBase = codebase;
    return true;
}

bool ExtensionManager::updateCheckEnabled()
{
    return QSettings().value(QLatin1String("extensions/autoUpdateCheck"),
                             false).toBool();
}

void ExtensionManager::setUpdateCheckEnabled(bool enabled)
{
    QSettings().setValue(QLatin1String("extensions/autoUpdateCheck"),
                         enabled);
}

bool ExtensionManager::updateCheckInProgress() const
{
    return m_updateCheckRunning;
}

ExtensionManager::UpdateResult
ExtensionManager::updateResultFor(const QString &id) const
{
    return m_lastUpdateResults.value(id);
}

QString ExtensionManager::updateUrlFor(const QString &id) const
{
    const QString cached = m_updateUrls.value(id);
    if (!cached.isEmpty())
        return cached;
#if QT_CONFIG(webengine_extensions)
    if (QWebEngineExtensionManager *extensions = managerFor(m_profiles)) {
        const QList<QWebEngineExtensionInfo> loaded = extensions->extensions();
        for (const QWebEngineExtensionInfo &info : loaded) {
            if (info.id() != id || info.path().isEmpty())
                continue;
            const Manifest manifest = inspectManifest(info.path());
            return manifest.valid ? manifest.updateUrl : QString();
        }
    }
#endif
    return QString();
}

void ExtensionManager::checkForUpdates(bool manual)
{
    if (m_updateCheckRunning) {
        emit errorOccurred(tr("An extension update check is already in progress."));
        return;
    }
    m_updateCheckRunning = true;
    m_updateCheckManual = manual;
    m_updateResults.clear();
    emit updateCheckStarted();

#if QT_CONFIG(webengine_extensions)
    const QList<ExtensionInfo> list = extensions();
    for (const ExtensionInfo &info : list) {
        // Component extensions ship inside the profile and cannot
        // self-update; session-loaded and installed extensions with a
        // source dir can.
        if (info.builtin || info.path.isEmpty())
            continue;
        UpdateResult result;
        result.id = info.id;
        result.name = info.name.isEmpty() ? info.id : info.name;
        const Manifest manifest = inspectManifest(info.path);
        if (manifest.valid)
            result.currentVersion = manifest.version;
        const QString updateUrl = manifest.updateUrl;
        m_updateUrls.insert(info.id, updateUrl);
        const QUrl url(updateUrl);
        const bool fetchable =
            url.scheme() == QLatin1String("https")
            || url.scheme() == QLatin1String("http");
        if (updateUrl.isEmpty() || !fetchable) {
            result.noSource = true;
            if (!updateUrl.isEmpty())
                result.error = tr("update_url \"%1\" is not http(s)")
                    .arg(updateUrl);
            m_updateResults.append(result);
            m_lastUpdateResults.insert(result.id, result);
            continue;
        }
        fetchUpdateManifest(result, url);
    }
#endif
    if (m_updateJobs.isEmpty())
        finishUpdateCheck();
}

void ExtensionManager::fetchUpdateManifest(const UpdateResult &result,
                                           const QUrl &updateUrl)
{
    UpdateJob job;
    job.result = result;
    job.updateUrl = updateUrl;

    QNetworkRequest request(updateUrl);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = NetworkAccessManager::instance()->get(request);
    reply->setParent(this);
    connect(reply, &QNetworkReply::readyRead, this, [reply]() {
        if (reply->bytesAvailable() > kUpdateManifestMaxBytes)
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        UpdateJob job = m_updateJobs.take(reply);
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            job.result.error = tr("update manifest fetch failed: %1")
                .arg(reply->errorString());
        } else if (parseUpdateManifest(reply->readAll(),
                                       job.result.id, &job.result)
                   && job.result.error.isEmpty()
                   && !job.result.upToDate) {
            // A newer version was offered — fetch the package.
            fetchUpdatePackage(job.result);
            return;
        }
        m_updateResults.append(job.result);
        m_lastUpdateResults.insert(job.result.id, job.result);
        finishUpdateCheck();
    });
    QTimer::singleShot(kUpdateFetchTimeoutMs, reply, [reply]() {
        if (reply->isRunning())
            reply->abort();
    });
    m_updateJobs.insert(reply, job);
}

void ExtensionManager::fetchUpdatePackage(const UpdateResult &result)
{
    UpdateJob job;
    job.result = result;
    job.updateUrl = result.codeBase;

    QNetworkRequest request(result.codeBase);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = NetworkAccessManager::instance()->get(request);
    reply->setParent(this);
    connect(reply, &QNetworkReply::readyRead, this, [reply]() {
        if (reply->bytesAvailable() > kUpdatePackageMaxBytes)
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        UpdateJob job = m_updateJobs.take(reply);
        reply->deleteLater();

        if (reply->error() == QNetworkReply::NoError) {
            const QByteArray body = reply->readAll();
            QString fileName =
                QFileInfo(job.result.codeBase.path()).fileName();
            fileName.remove(QRegularExpression(
                QLatin1String("[^A-Za-z0-9._-]")));
            if (fileName.isEmpty()
                || fileName == QLatin1String(".")
                || fileName == QLatin1String(".."))
                fileName = QLatin1String("package.zip");
            const QString dirPath = updatesPath()
                + QLatin1Char('/') + job.result.id;
            QDir().mkpath(dirPath);
            const QString saved = dirPath + QLatin1Char('/') + fileName;
            QSaveFile file(saved);
            if (file.open(QIODevice::WriteOnly)
                && file.write(body) == body.size() && file.commit()) {
                job.result.savedTo = saved;
                // Qt's installer accepts unpacked dirs and .zip — a
                // .crx (or anything else) is kept for manual install.
                const bool isZip = body.startsWith("PK\x03\x04")
                    || fileName.endsWith(QLatin1String(".zip"),
                                         Qt::CaseInsensitive);
                if (isZip && m_updateCheckManual) {
                    job.result.installTriggered = true;
                    installExtension(saved);
                }
            } else {
                job.result.error = tr("could not save update package to %1")
                    .arg(saved);
            }
        } else {
            job.result.error = tr("update package download failed: %1")
                .arg(reply->errorString());
        }
        m_updateResults.append(job.result);
        m_lastUpdateResults.insert(job.result.id, job.result);
        if (!job.result.availableVersion.isEmpty())
            emit updateAvailable(job.result);
        finishUpdateCheck();
    });
    QTimer::singleShot(kUpdateDownloadTimeoutMs, reply, [reply]() {
        if (reply->isRunning())
            reply->abort();
    });
    m_updateJobs.insert(reply, job);
}

void ExtensionManager::finishUpdateCheck()
{
    if (m_updateJobs.isEmpty() && m_updateCheckRunning) {
        m_updateCheckRunning = false;
        QSettings().setValue(QLatin1String("extensions/lastUpdateCheck"),
                             QDateTime::currentSecsSinceEpoch());
        emit updateCheckFinished(m_updateResults);
    }
}

void ExtensionManager::scheduleAutoUpdateCheck()
{
    if (m_autoUpdateScheduled)
        return;
    m_autoUpdateScheduled = true;
    // First check shortly after startup, then daily — the QSettings
    // timestamp gates both to one check per 20 hours.
    QTimer::singleShot(45000, this, [this]() {
        maybeAutoUpdateCheck();
    });
    QTimer *daily = new QTimer(this);
    daily->setInterval(24 * 60 * 60 * 1000);
    connect(daily, &QTimer::timeout, this, [this]() {
        maybeAutoUpdateCheck();
    });
    daily->start();
}

void ExtensionManager::maybeAutoUpdateCheck()
{
    if (!updateCheckEnabled() || m_updateCheckRunning)
        return;
    const qint64 last = QSettings()
        .value(QLatin1String("extensions/lastUpdateCheck"), 0).toLongLong();
    if (QDateTime::currentSecsSinceEpoch() - last < 20 * 60 * 60)
        return;
    checkForUpdates(false);
}
