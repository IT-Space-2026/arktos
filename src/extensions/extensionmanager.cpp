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

#include "browserapplication.h"
#include "extensionbackgroundhost.h"
#include "browserpaths.h"
#include "browserprofile.h"
#include "networkaccessmanager.h"

#if defined(ARORA_RUSTCORE)
#include "rustcore.h"
#endif

#include <qcoreapplication.h>
#include <qcryptographichash.h>
#include <qdatetime.h>
#include <qdebug.h>
#include <qdir.h>
#include <qdiriterator.h>
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

#include <qwebenginepage.h>
#include <qwebengineprofile.h>
#include <qwebenginescript.h>
#include <qwebenginescriptcollection.h>

#include <qwebengineextensionmanager.h>

#include <memory>

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
static QWebEngineExtensionManager *managerFor(
        const QList<QPointer<QWebEngineProfile>> &profiles)
{
    for (const QPointer<QWebEngineProfile> &profile : profiles) {
        if (profile && !profile->isOffTheRecord() && profile->extensionManager())
            return profile->extensionManager();
    }
    return nullptr;
}

// The single by-id lookup — removeExtension, setExtensionEnabled and
// updateUrlFor each had their own copy of this loop (EXT05 cleanup).
// Returns an invalid info (empty id) when the id is not loaded.
static QWebEngineExtensionInfo findById(
        QWebEngineExtensionManager *extensions, const QString &id)
{
    if (extensions) {
        const QList<QWebEngineExtensionInfo> loaded =
            extensions->extensions();
        for (const QWebEngineExtensionInfo &info : loaded) {
            if (info.id() == id)
                return info;
        }
    }
    return QWebEngineExtensionInfo();
}

#endif // QT_CONFIG(webengine_extensions)

namespace {
// A real reply (or picked package) buffers whatever the peer sends;
// cap rather than letting a hostile endpoint grow memory unbounded.
// kUpdatePackageMaxBytes doubles as the .zip inspection bound.
constexpr qint64 kUpdateManifestMaxBytes = 1024 * 1024;
constexpr qint64 kUpdatePackageMaxBytes = 64 * 1024 * 1024;
constexpr int kUpdateFetchTimeoutMs = 20000;
constexpr int kUpdateDownloadTimeoutMs = 60000;

// EXT05: update fetches are https-only.  Loopback http stays allowed
// so local fixtures and dev servers keep working — a remote http URL
// is refused at fetch time, not just at parse.
static bool isFetchableUpdateUrl(const QUrl &url)
{
    if (url.scheme() == QLatin1String("https"))
        return true;
    if (url.scheme() != QLatin1String("http"))
        return false;
    const QString host = url.host().toLower();
    return host == QLatin1String("localhost")
        || host == QLatin1String("127.0.0.1")
        || host == QLatin1String("::1")
        || host == QLatin1String("[::1]");
}
}

#if defined(ARORA_RUSTCORE)
// EXT06: the review dialog consumes PRE-VALIDATED manifest fields —
// rc_ext_manifest_check parses the schema + permission lists in Rust
// and this maps its verdict onto the UI-facing Manifest.  The raw
// manifest.json bytes never reach Qt JSON objects.
static void fillManifestFromCheck(ExtensionManager::Manifest *manifest,
                                  const QJsonObject &check)
{
    manifest->valid = check.value(QLatin1String("valid")).toBool();
    manifest->manifestVersion =
        check.value(QLatin1String("manifest_version")).toInt();
    manifest->name = check.value(QLatin1String("name")).toString();
    manifest->version = check.value(QLatin1String("version")).toString();
    manifest->description =
        check.value(QLatin1String("description")).toString();
    manifest->hasBackground =
        check.value(QLatin1String("has_background")).toBool();
    manifest->hasAction = check.value(QLatin1String("has_action")).toBool();
    manifest->contentScriptCount =
        check.value(QLatin1String("content_script_count")).toInt();
    manifest->updateUrl = check.value(QLatin1String("update_url")).toString();

    const auto stringList = [&check](const char *key) {
        QStringList list;
        const QJsonArray entries =
            check.value(QLatin1String(key)).toArray();
        for (const QJsonValue &value : entries)
            list.append(value.toString());
        return list;
    };
    manifest->backgroundScripts = stringList("background_scripts");
    manifest->permissions = stringList("permissions");
    manifest->hostPermissions = stringList("host_permissions");
    manifest->unsupported = stringList("unsupported");
    manifest->unverified = stringList("unverified");
    manifest->dangerous = stringList("dangerous");
    manifest->error = stringList("errors").join(QLatin1String(" "));
}

// Packaged install path: the whole archive is verified in Rust —
// structure, member names, declared-hash — and its embedded
// manifest.json comes back already classified.
static ExtensionManager::Manifest inspectPackageManifest(const QString &path)
{
    ExtensionManager::Manifest manifest;
    QFile file(path);
    if (file.size() > kUpdatePackageMaxBytes
            || !file.open(QIODevice::ReadOnly)) {
        manifest.error = QObject::tr("cannot read %1").arg(path);
        return manifest;
    }
    const QByteArray body = file.readAll();
    RcBuffer out{};
    const RcStatus verified = rc_ext_verify_package(
        reinterpret_cast<const uint8_t *>(body.constData()),
        size_t(body.size()), nullptr, nullptr, 0, &out);
    if (verified != RC_OK) {
        char *message = rc_last_error_message();
        manifest.error = QObject::tr("package verification failed: %1")
            .arg(message ? QString::fromUtf8(message)
                         : QObject::tr("error"));
        rc_string_free(message);
        return manifest;
    }
    const QJsonObject verdict = QJsonDocument::fromJson(
        QByteArray(reinterpret_cast<const char *>(out.data),
                   qsizetype(out.len))).object();
    rc_buffer_free(out);
    if (verdict.value(QLatin1String("status")).toString()
            != QLatin1String("valid")) {
        manifest.error = QObject::tr("package rejected: %1")
            .arg(verdict.value(QLatin1String("error")).toString());
        return manifest;
    }
    const QJsonObject check =
        verdict.value(QLatin1String("manifest")).toObject();
    if (check.isEmpty() || !check.value(QLatin1String("valid")).toBool()) {
        manifest.error = QObject::tr(
            "package contains no readable manifest.json");
        return manifest;
    }
    fillManifestFromCheck(&manifest, check);
    // Honesty note for the review dialog: v1 verifies the CRX3
    // header's structure, not the signatures themselves.
    manifest.packageNote =
        verdict.value(QLatin1String("signing")).toString()
                == QLatin1String("signed")
        ? QObject::tr("CRX3 package carrying a signature block — "
                      "the signature is structure-checked, not "
                      "cryptographically verified.")
        : QObject::tr("Package is unsigned — its contents were "
                      "structure-checked only.");
    return manifest;
}
#endif // ARORA_RUSTCORE

ExtensionManager::ExtensionManager(QObject *parent)
    : QObject(parent)
{
    // EXT12: every lifecycle pulse (load/install/uninstall + the
    // enable/disable settle) fans out through changed() — coalesce
    // them into one deferred scan that queues health probes for any
    // enabled background-carrying extension not yet measured this
    // session.  Probes are strictly additive: nothing already
    // verdicted re-queues here (setExtensionEnabled clears the stale
    // verdict itself before re-enabling).
    connect(this, &ExtensionManager::changed, this, [this]() {
        if (m_healthScanScheduled)
            return;
        m_healthScanScheduled = true;
        QTimer::singleShot(1500, this, [this]() {
            m_healthScanScheduled = false;
            enqueuePendingProbes();
        });
    });
    // A live probe page must not outlive teardown — an orphaned
    // WebEnginePage at profile destruction is exactly the
    // "WebEnginePage still not deleted" crash shape (MIG12 notes).
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
            this, [this]() {
        m_healthQueue.clear();
        delete m_healthPage;
    });
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
    // EXT05 hard refusal — in a tor-mode process no profile is wired
    // at all, and the tor profile itself is refused even from a
    // non-tor caller: extensions and injected scripts are a
    // deanonymization surface (a beaconing extension unmasks the
    // user).  prepareProfile() already skips tor mode; this gate
    // keeps the invariant true for every other caller.
    if (BrowserApplication::isTorMode()
            || profile == BrowserProfile::torProfileIfCreated())
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
            this, [this, profile](const QWebEngineExtensionInfo &info) {
        const ExtensionInfo plain = infoFrom(info);
        finishBundledInstall(plain);
        // SEC25: pin the installed tree.  An applied update lands here
        // too, re-pinning its new files as the baseline the next
        // integrity sweep compares against.
        pinInstalledExtension(profile, plain);
        emit extensionInstalled(plain);
        emit changed();
    });
    connect(extensions, &QWebEngineExtensionManager::unloadFinished,
            this, relay(&ExtensionManager::extensionUnloaded));
    connect(extensions, &QWebEngineExtensionManager::uninstallFinished,
            this, [this, profile](const QWebEngineExtensionInfo &info) {
        const ExtensionInfo plain = infoFrom(info);
        // SEC25: the pin and any tamper flag die with the extension.
        QSettings().remove(integritySettingsKey(
            profile->storageName(), plain.id));
        m_tamperedIds.remove(plain.id);
        // EXT12: its health verdict too.
        m_healthVerdicts.remove(plain.id);
        m_healthDetails.remove(plain.id);
        emit extensionUninstalled(plain);
        emit changed();
    });

    // SEC25: once the engine finishes enumerating the profile's
    // installed extensions, compare each on-disk tree against its
    // install-time pin and flag anything that changed underneath us.
    scheduleIntegritySweep(profile, 0);

    // EXT14: the hosted-background runtime reconciles against the
    // engine's enabled/loaded state on every change.
    static bool runtimeWired = false;
    if (!runtimeWired) {
        runtimeWired = true;
        ExtensionBackgroundRuntime *runtime =
            ExtensionBackgroundRuntime::instance();
        connect(this, &ExtensionManager::changed,
                runtime, &ExtensionBackgroundRuntime::syncNow);
    }
    if (!qEnvironmentVariableIsSet("ARORA_EXTHOST_OFF"))
        ExtensionBackgroundRuntime::instance()->attachProfile(profile);

    scheduleAutoUpdateCheck();

    // EXT07: default-installed bundle lands once per profile.
    installBundledExtensions(profile);

    // EXT12: the engine's installed-extension registry fills in
    // asynchronously, so the first health scan runs deferred; every
    // later lifecycle pulse re-scans through the changed() coalescer.
    const QPointer<QWebEngineProfile> scanGuard(profile);
    QTimer::singleShot(5000, this, [this, scanGuard]() {
        if (scanGuard)
            enqueuePendingProbes();
    });
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
    for (const QWebEngineExtensionInfo &info : loaded) {
        ExtensionInfo plain = infoFrom(info);
        plain.tampered = m_tamperedIds.contains(info.id());
        // EXT12: fold the manifest's declared-but-missing chrome.*
        // APIs together with the measured background verdict into
        // one health state — the Preferences row reads this instead
        // of a naked "Enabled".
        bool hasBackground = false;
        if (!plain.builtin && !plain.path.isEmpty()) {
            const Manifest manifest = inspectManifest(plain.path);
            if (manifest.valid) {
                plain.missingApis = manifest.unsupported;
                hasBackground = manifest.hasBackground;
            }
        }
        plain.health = classifyHealth(hasBackground, plain.missingApis,
                m_healthVerdicts.value(plain.id, ProbeVerdict::Pending),
                &plain.healthReason);
        list.append(plain);
    }
    // EXT14: fold in the hosted-runtime tier — a Ready host is the
    // "Hosted" surface; Starting/Failed report through hostState and
    // hostError so the UI and smokes can tell a live background from
    // a dead one without poking the engine.
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();
    for (ExtensionInfo &info : list) {
        ExtensionBackgroundHost *host = runtime->host(info.id);
        if (!host)
            continue;
        switch (host->state()) {
        case ExtensionBackgroundHost::State::Starting:
            info.hostState = QStringLiteral("starting");
            break;
        case ExtensionBackgroundHost::State::Ready:
            info.hostState = QStringLiteral("hosted");
            info.hosted = true;
            break;
        case ExtensionBackgroundHost::State::Failed:
            info.hostState = QStringLiteral("failed");
            break;
        }
        info.hostError = host->lastError();
    }
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

void ExtensionManager::grantConsent(const QString &path)
{
    if (!path.isEmpty())
        m_consentedPaths.insert(path);
}

bool ExtensionManager::hasConsent(const QString &path) const
{
    return m_consentedPaths.contains(path);
}

void ExtensionManager::loadExtension(const QString &path)
{
    // EXT05: a silent load is a bug — the path must have passed the
    // consent gate first (ExtensionReviewDialog or an explicit grant).
    if (!m_consentedPaths.remove(path)) {
        emit errorOccurred(tr("Refusing to load %1 — extension consent "
                              "was not granted.").arg(path));
        return;
    }
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
    // Same consent gate as loadExtension() — a package must never
    // reach the profile without explicit approval (EXT05).
    if (!m_consentedPaths.remove(path)) {
        emit errorOccurred(tr("Refusing to install %1 — extension consent "
                              "was not granted.").arg(path));
        return;
    }
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
        const QWebEngineExtensionInfo info = findById(extensions, id);
        if (!info.id().isEmpty()) {
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
        const QWebEngineExtensionInfo info = findById(extensions, id);
        if (!info.id().isEmpty()) {
            extensions->setExtensionEnabled(info, enabled);
            // Qt exposes no enabledChanged signal — poll briefly until
            // the engine reports the new state (or the extension
            // disappears), then refresh.  A settled poll beats the old
            // blind fixed-delay refresh: the UI updates as soon as the
            // toggle lands and never waits on a slow one.
            QPointer<QWebEngineExtensionManager> engine(extensions);
            QTimer *poll = new QTimer(this);
            poll->setInterval(50);
            auto tries = std::make_shared<int>(0);
            connect(poll, &QTimer::timeout, this,
                    [this, poll, engine, id, enabled, tries]() {
                const QWebEngineExtensionInfo current =
                    findById(engine.data(), id);
                const bool settled = current.id().isEmpty()
                    || current.isEnabled() == enabled;
                if (!settled && ++*tries < 40)
                    return;
                poll->stop();
                poll->deleteLater();
                // EXT12: a fresh enable re-measures — clear the stale
                // verdict so the changed() scan re-queues the probe.
                if (enabled) {
                    m_healthVerdicts.remove(id);
                    m_healthDetails.remove(id);
                }
                emit changed();
            });
            poll->start();
            return;
        }
        emit errorOccurred(tr("Extension %1 is no longer loaded.").arg(id));
        return;
    }
#endif
    emit errorOccurred(tr("Extension support is not available in this build of Qt WebEngine."));
}

QString ExtensionManager::bundledExtensionsPath()
{
    const QString overrideDir = QString::fromLocal8Bit(
        qgetenv("ARORA_BUNDLED_EXTENSIONS_DIR"));
    if (!overrideDir.isEmpty())
        return overrideDir;
    const QString installed = BrowserApplication::installedDataDirectory()
        + QLatin1String("/extensions");
    if (QFileInfo(installed).isDir())
        return installed;
    // Uninstalled build: the binary links into either src/ or the
    // repository root depending on the qmake target layout.
    const QString appDir = QCoreApplication::applicationDirPath();
    for (const QString &rel : {QStringLiteral("/data/extensions"),
                               QStringLiteral("/src/data/extensions")}) {
        const QString candidate = appDir + rel;
        if (QFileInfo(candidate).isDir())
            return candidate;
    }
    return QString();
}

#if QT_CONFIG(webengine_extensions)
// "bitwarden-2026.9.3.zip" -> "bitwarden" — the loose product-name
// stem used to correlate a bundled file with the extension Chromium
// reports back (manifest "name" localizes, so the raw package base
// name is the stable side of the match).
static QString bundledStem(const QString &fileName)
{
    QString stem = fileName;
    if (stem.endsWith(QLatin1String(".zip"), Qt::CaseInsensitive))
        stem.chop(4);
    stem.remove(QRegularExpression(
        QStringLiteral("[-_][0-9][0-9.]*(-[A-Za-z0-9.]+)?$")));
    return stem;
}
#endif

void ExtensionManager::installBundledExtensions(QWebEngineProfile *profile)
{
#if QT_CONFIG(webengine_extensions)
    if (!profile || profile->isOffTheRecord())
        return;
    QWebEngineExtensionManager *extensions = profile->extensionManager();
    if (!extensions)
        return;
    const QString dirPath = bundledExtensionsPath();
    if (dirPath.isEmpty())
        return;
    const QDir dir(dirPath);
    QStringList names = dir.entryList(
        QStringList() << QLatin1String("*.zip"), QDir::Files, QDir::Name);
    const QFileInfoList subdirs = dir.entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &sub : subdirs) {
        if (QFileInfo::exists(sub.absoluteFilePath()
                + QLatin1String("/manifest.json")))
            names.append(sub.fileName());
    }
    if (names.isEmpty())
        return;

    // The consumed marker is one-shot per profile: once a bundled
    // file has installed (or been satisfied by a same-named existing
    // extension) it never installs again — a user's uninstall or
    // disable is a decision, not a state to repair.
    const QString settingsKey = QLatin1String("extensions/bundled/")
        + (profile->storageName().isEmpty()
           ? QStringLiteral("default") : profile->storageName());
    QSettings settings;
    const QStringList consumed = settings.value(settingsKey).toStringList();
    QStringList satisfied;

    for (const QString &name : names) {
        if (consumed.contains(name))
            continue;
        const QString stem = bundledStem(name);
        if (!stem.isEmpty()) {
            const QList<QWebEngineExtensionInfo> existing =
                extensions->extensions();
            for (const QWebEngineExtensionInfo &info : existing) {
                if (info.isInstalled()
                        && info.name().contains(stem, Qt::CaseInsensitive)) {
                    satisfied.append(name);
                    break;
                }
            }
            if (satisfied.contains(name))
                continue;
        }

        // "Extension-fresh" = the profile's installPath holds nothing
        // yet.  Fresh profiles get the bundle enabled as part of the
        // default install — that is the "installed by default" the
        // user asked for.  Profiles that already curated extensions
        // get the bundle installed-but-disabled so the choice to
        // enable stays theirs; nothing already installed is ever
        // force-enabled retroactively (EXT07).
        const QString installDir = extensions->installPath();
        const bool fresh = installDir.isEmpty()
            || !QDir(installDir).exists()
            || QDir(installDir).entryList(
                QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty();

        PendingBundle pending;
        pending.engine = extensions;
        pending.settingsKey = settingsKey;
        pending.fileName = name;
        pending.stem = stem;
        pending.enable = fresh;
        m_bundledPending.append(pending);

        // Consent: bundling IS Arora's approval — the grant is made
        // programmatically on this internal path but still flows
        // through the one-shot gate (consumed by this install), so a
        // manual retry of the same path later needs a real review.
        const QString path = dir.absoluteFilePath(name);
        grantConsent(path);
        installExtension(path);
    }
    if (!satisfied.isEmpty())
        settings.setValue(settingsKey, consumed + satisfied);
#endif
}

void ExtensionManager::finishBundledInstall(const ExtensionInfo &info)
{
#if QT_CONFIG(webengine_extensions)
    for (int i = 0; i < m_bundledPending.size(); ++i) {
        const PendingBundle pending = m_bundledPending.at(i);
        if (pending.engine.isNull()) {
            m_bundledPending.removeAt(i--);
            continue;
        }
        // Correlate loosely by name stem; on an install error the
        // reported name can be empty, so fall back to the oldest
        // pending entry (bundled installs issue serially at startup,
        // before anything else could reasonably race in).
        const bool stemMatch = !pending.stem.isEmpty()
            && info.name.contains(pending.stem, Qt::CaseInsensitive);
        const bool errorFallback = !info.error.isEmpty() && i == 0;
        if (!stemMatch && !errorFallback)
            continue;
        m_bundledPending.removeAt(i);
        if (!info.error.isEmpty() || !info.installed)
            return;
        QSettings settings;
        QStringList consumed =
            settings.value(pending.settingsKey).toStringList();
        if (!consumed.contains(pending.fileName)) {
            consumed.append(pending.fileName);
            settings.setValue(pending.settingsKey, consumed);
        }
        if (pending.enable) {
            const QWebEngineExtensionInfo ext =
                findById(pending.engine, info.id);
            if (!ext.id().isEmpty())
                pending.engine->setExtensionEnabled(ext, true);
        }
        return;
    }
#else
    Q_UNUSED(info);
#endif
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
#if defined(ARORA_RUSTCORE)
        // EXT06: the archive is verified in Rust and its embedded
        // manifest comes back pre-classified — the review dialog
        // shows real permissions for packages now.
        return inspectPackageManifest(info.absoluteFilePath());
#else
        // Qt's installer accepts .zip packages; the manifest inside is
        // not inspected here — Chromium reports errors through
        // installFinished's error string instead.
        manifest.error = tr("zip package — manifest not inspected before install");
        return manifest;
#endif
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
#if defined(ARORA_RUSTCORE)
    // EXT06: schema + permissions are parsed in Rust — the fields
    // below come back pre-classified, never from raw JSON on this side.
    const QByteArray raw = file.readAll();
    RcBuffer out{};
    const RcStatus checked = rc_ext_manifest_check(
        reinterpret_cast<const uint8_t *>(raw.constData()),
        size_t(raw.size()), &out);
    if (checked != RC_OK) {
        char *message = rc_last_error_message();
        manifest.error = tr("manifest.json is not valid JSON: %1")
            .arg(message ? QString::fromUtf8(message)
                         : tr("malformed document"));
        rc_string_free(message);
        return manifest;
    }
    const QJsonObject check = QJsonDocument::fromJson(
        QByteArray(reinterpret_cast<const char *>(out.data),
                   qsizetype(out.len))).object();
    rc_buffer_free(out);
    fillManifestFromCheck(&manifest, check);
    return manifest;
#else
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
    // EXT14: same field order as the Rust classifier — scripts[] in
    // listed order, then service_worker.
    const QJsonObject background =
        root.value(QLatin1String("background")).toObject();
    for (const QJsonValue &entry
         : background.value(QLatin1String("scripts")).toArray()) {
        const QString name = entry.toString();
        if (!name.isEmpty())
            manifest.backgroundScripts.append(name);
    }
    const QString worker =
        background.value(QLatin1String("service_worker")).toString();
    if (!worker.isEmpty())
        manifest.backgroundScripts.append(worker);
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
    const QStringList dangerous = dangerousPermissions();
    const QStringList dangerousHosts = dangerousHostPatterns();
    for (const QString &permission : manifest.permissions) {
        if (unsupported.contains(permission))
            manifest.unsupported.append(permission);
        else if (unverified.contains(permission))
            manifest.unverified.append(permission);
        if (dangerous.contains(permission))
            manifest.dangerous.append(permission);
    }
    for (const QString &pattern : manifest.hostPermissions) {
        if (dangerousHosts.contains(pattern))
            manifest.dangerous.append(pattern);
    }

    if (manifest.manifestVersion != 3)
        manifest.error = tr("Qt WebEngine supports Manifest V3 extensions only "
                            "(this package declares version %1).")
            .arg(manifest.manifestVersion);
#endif // ARORA_RUSTCORE
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

QStringList ExtensionManager::dangerousPermissions()
{
    // EXT05 deny-by-default consent set — each of these can take over
    // the browser or the machine if the engine honors it, so the
    // review dialog requires an explicit per-permission checkbox
    // instead of a passive warning.  Mirrors DANGEROUS_PERMISSIONS in
    // rustcore's extverify.rs — keep the lists in sync.
    static const QStringList dangerous = {
        QStringLiteral("nativeMessaging"),
        QStringLiteral("debugger"),
        QStringLiteral("webRequestBlocking"),
        QStringLiteral("cookies"),
        QStringLiteral("browsingData"),
    };
    return dangerous;
}

QStringList ExtensionManager::dangerousHostPatterns()
{
    // Host-permission spellings of "every website" — mirrors
    // DANGEROUS_HOST_PATTERNS in extverify.rs.
    static const QStringList dangerous = {
        QStringLiteral("<all_urls>"),
        QStringLiteral("*://*/*"),
    };
    return dangerous;
}

ExtensionManager::ExtensionHealth ExtensionManager::classifyHealth(
        bool hasBackground, const QStringList &missingApis,
        ProbeVerdict verdict, QString *reason)
{
    QString why;
    // A dead background means the extension cannot work at all —
    // every declared feature that routes through the worker (the
    // whole vault in Bitwarden's case) is dead with it.
    if (verdict == ProbeVerdict::Dead) {
        why = missingApis.isEmpty()
            ? tr("its background service worker never became ready")
            : tr("its background service worker never became ready — "
                 "it needs chrome.* APIs Qt WebEngine doesn't provide "
                 "(%1)").arg(missingApis.join(QLatin1String(", ")));
    } else if (!missingApis.isEmpty()) {
        why = tr("it declares chrome.* APIs Qt WebEngine doesn't "
                 "provide (%1) — the features using them won't work")
                  .arg(missingApis.join(QLatin1String(", ")));
    }
    if (reason)
        *reason = why;

    if (verdict == ProbeVerdict::Dead)
        return ExtensionHealth::NotSupported;
    if (!missingApis.isEmpty())
        return ExtensionHealth::Limited;
    if (verdict == ProbeVerdict::Responsive
            || (verdict == ProbeVerdict::None && !hasBackground))
        return ExtensionHealth::Healthy;
    return ExtensionHealth::Unknown;
}

bool ExtensionManager::isBroadHostPattern(const QString &pattern)
{
    if (pattern == QLatin1String("<all_urls>"))
        return true;
    // "scheme://host/path" — a bare "*" host grants the extension the
    // whole scheme, covering "*://*/*" plus the http/https (and any
    // other scheme's) wildcard roots.  "*.host" subdomain wildcards
    // stay scoped to one site and are not broad.
    const int schemeSep = pattern.indexOf(QLatin1String("://"));
    if (schemeSep < 0)
        return false;
    const int slash = pattern.indexOf(QLatin1Char('/'), schemeSep + 3);
    const QString host = slash < 0
        ? pattern.mid(schemeSep + 3)
        : pattern.mid(schemeSep + 3, slash - schemeSep - 3);
    return host == QLatin1String("*");
}

// -- SEC25: install-integrity pin -----------------------------------------
//
// Every successful install records a sha256 over the extension's
// on-disk file tree (manifest.json, service workers, content scripts,
// resources — everything Chromium executes or serves).  The sweep
// that runs right after a profile is wired re-hashes each installed
// dir and compares: a mismatch means something outside the engine
// changed the files, so the extension is flagged — ExtensionInfo::
// tampered plus the extensionTampered signal — with the honest
// "files changed since install — review or remove" wording instead
// of silently loading.  It is not unloaded or disabled: the engine
// still owns that decision and the user makes it.  Uninstall clears
// the pin; an applied update re-pins through installFinished.

QString ExtensionManager::integritySettingsKey(const QString &storageName,
                                               const QString &id)
{
    return QLatin1String("extensions/integrity/")
        + (storageName.isEmpty()
           ? QStringLiteral("default") : storageName)
        + QLatin1Char('/') + id;
}

QByteArray ExtensionManager::extensionTreeHash(const QString &dirPath)
{
    const QDir dir(dirPath);
    if (!dir.exists())
        return QByteArray();
    // Deterministic: "<relpath>\0<contents>\0" per file in sorted
    // path order, so identical trees hash identically across runs.
    QDirIterator it(dirPath,
        QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDirIterator::Subdirectories);
    QStringList relPaths;
    while (it.hasNext())
        relPaths.append(dir.relativeFilePath(it.next()));
    relPaths.sort();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const QString &rel : relPaths) {
        hash.addData(rel.toUtf8());
        hash.addData(QByteArrayView("\0", 1));
        QFile file(dir.absoluteFilePath(rel));
        if (file.open(QIODevice::ReadOnly)) {
            while (!file.atEnd())
                hash.addData(file.read(64 * 1024));
        }
        // An unreadable file still contributes its name — a file
        // readable at pin time that no longer is changes the hash.
        hash.addData(QByteArrayView("\0", 1));
    }
    return hash.result().toHex();
}

void ExtensionManager::pinInstalledExtension(QWebEngineProfile *profile,
                                             const ExtensionInfo &info)
{
    if (!profile || !info.installed || info.builtin
            || info.path.isEmpty() || !info.error.isEmpty())
        return;
    const QByteArray hash = extensionTreeHash(info.path);
    if (hash.isEmpty())
        return;
    QSettings().setValue(
        integritySettingsKey(profile->storageName(), info.id),
        QString::fromLatin1(hash));
}

void ExtensionManager::scheduleIntegritySweep(QWebEngineProfile *profile,
                                              int attempt)
{
    const QPointer<QWebEngineProfile> guard(profile);
    QTimer::singleShot(attempt ? 250 : 0, this,
                       [this, guard, attempt]() {
        if (!guard)
            return;
#if QT_CONFIG(webengine_extensions)
        QWebEngineExtensionManager *extensions =
            guard->extensionManager();
        // Chromium's installed-extension registry fills in
        // asynchronously — when the install dir holds entries the
        // list will catch up, so retry briefly instead of sweeping a
        // not-yet-populated view and calling a tampered tree clean.
        if (attempt < 20 && extensions
                && extensions->extensions().isEmpty()
                && !QDir(extensions->installPath()).entryList(
                        QDir::Dirs | QDir::NoDotAndDotDot).isEmpty()) {
            scheduleIntegritySweep(guard, attempt + 1);
            return;
        }
#endif
        verifyInstalledIntegrity(guard);
    });
}

void ExtensionManager::verifyInstalledIntegrity(QWebEngineProfile *profile)
{
#if QT_CONFIG(webengine_extensions)
    if (!profile || profile->isOffTheRecord())
        return;
    QWebEngineExtensionManager *extensions = profile->extensionManager();
    if (!extensions)
        return;
    const QString storage = profile->storageName();
    const QList<QWebEngineExtensionInfo> list = extensions->extensions();
    for (const QWebEngineExtensionInfo &info : list) {
        if (!info.isInstalled() || info.path().isEmpty()
                || !info.error().isEmpty())
            continue;
        ExtensionInfo plain = infoFrom(info);
        if (plain.builtin)
            continue;
        const QString key = integritySettingsKey(storage, info.id());
        QSettings settings;
        const QString pinned = settings.value(key).toString();
        const QString current =
            QString::fromLatin1(extensionTreeHash(info.path()));
        if (pinned.isEmpty()) {
            // Baseline adoption: this install predates pinning (or
            // arrived before installFinished ran), so there is no
            // older truth to compare against — the current tree
            // becomes the baseline the next sweep checks.
            if (!current.isEmpty())
                settings.setValue(key, current);
            continue;
        }
        if (!current.isEmpty() && current != pinned
                && !m_tamperedIds.contains(plain.id)) {
            m_tamperedIds.insert(plain.id);
            plain.tampered = true;
            emit errorOccurred(tr(
                "Extension \"%1\": files changed since install — "
                "review or remove.")
                .arg(plain.name.isEmpty() ? plain.id : plain.name));
            emit extensionTampered(plain);
            emit changed();
        }
    }
#else
    Q_UNUSED(profile);
#endif
}

// -- EXT12: background-liveness probe ----------------------------------
//
// EXT11 measured that a Manifest-V3 extension whose service worker
// dies during evaluation (Bitwarden: "Service worker registration
// failed. Status code: 15" — Chromium kErrorScriptEvaluateFailed on
// the missing chrome.* namespaces) leaves no signal the app can see
// through QWebEngineExtensionManager: the row still reads
// enabled+loaded, and its action popup spins on a worker that never
// exists.  The health probe measures it directly: a hidden page on
// the extension's own chrome-extension:// origin sends the worker a
// message and then counts BACKGROUND contexts via
// chrome.runtime.getContexts.  A worker that evaluated stays
// registered and answers (or refuses the ping with a fast lastError);
// a worker that eval-crashed retries forever and the ping times out.
//
// The probe page is the extension's own manifest.json — every
// extension ships one and it carries no extension JS, so measuring
// an extension never runs its code (a pinned popup's page could make
// network calls the user never asked for).  Only when the engine
// refuses to inject chrome.* bindings into that document does the
// probe fall back to the declared action popup; if neither context
// exposes chrome.runtime the verdict is left inconclusive rather
// than guessed — we never mark an extension dead without a
// measurement.

#if QT_CONFIG(webengine_extensions)
// Runs inside the extension-origin page.  Drives a timed
// sendMessage ping at the background context and then samples
// getContexts; the result lands on window.__aroraExtHealth for the
// C++ side to poll (runJavaScript cannot await a promise).
static const char kHealthProbeScript[] = R"JS(
(function () {
    var R = {runtime: false};
    function done() {
        try { window.__aroraExtHealth = JSON.stringify(R); }
        catch (e) { window.__aroraExtHealth = '{"runtime":false}'; }
    }
    try {
        R.runtime = (typeof chrome !== 'undefined')
            && !!(chrome.runtime && chrome.runtime.sendMessage);
    } catch (e) {}
    if (!R.runtime) { done(); return 'no-bindings'; }
    var settled = false;
    function contexts() {
        try {
            if (!chrome.runtime.getContexts) {
                R.backgrounds = -2;
                done();
                return;
            }
            chrome.runtime.getContexts({contextTypes: ['BACKGROUND']},
                function (cs) {
                    R.backgrounds = chrome.runtime.lastError
                        ? -1 : (cs ? cs.length : 0);
                    done();
                });
        } catch (e) { R.backgrounds = -1; done(); }
    }
    function finishPing(p) {
        R.ping = p;
        // A just-started worker lags a beat behind a fast lastError —
        // sample the context list after it settles.
        setTimeout(contexts, 1500);
    }
    var timer = setTimeout(function () {
        if (!settled) { settled = true; finishPing('TIMEOUT'); }
    }, 6000);
    try {
        chrome.runtime.sendMessage({__aroraHealthProbe: 1},
            function (resp) {
                if (settled) return;
                settled = true;
                clearTimeout(timer);
                var le = chrome.runtime.lastError;
                finishPing(le ? ('ERR:' + le.message)
                              : (resp === undefined ? 'EMPTY' : 'PONG'));
            });
    } catch (e) {
        if (!settled) {
            settled = true;
            clearTimeout(timer);
            finishPing('THROW:' + String(e));
        }
    }
    return 'started';
})()
)JS";
#endif

void ExtensionManager::enqueuePendingProbes()
{
#if QT_CONFIG(webengine_extensions)
    QWebEngineExtensionManager *engine = managerFor(m_profiles);
    if (!engine)
        return;
    QWebEngineProfile *profile = nullptr;
    for (const QPointer<QWebEngineProfile> &candidate : m_profiles) {
        if (candidate && candidate->extensionManager() == engine) {
            profile = candidate;
            break;
        }
    }
    if (!profile)
        return;
    for (const ExtensionInfo &info : extensions()) {
        if (!info.loaded || !info.enabled || info.builtin
                || info.path.isEmpty() || !info.error.isEmpty())
            continue;
        // A stored verdict (Pending in-flight, Responsive, Dead, or
        // None for the no-background/inconclusive cases) means the
        // extension was already measured this session — enable-
        // toggles clear it before re-queueing.
        if (m_healthVerdicts.contains(info.id))
            continue;
        if (inspectManifest(info.path).hasBackground)
            m_healthQueue.append(
                qMakePair(info.id, QPointer<QWebEngineProfile>(profile)));
        else
            // Nothing to measure — no background to be dead.
            m_healthVerdicts.insert(info.id, ProbeVerdict::None);
    }
    startNextHealthProbe();
#endif
}

void ExtensionManager::requestHealthProbe(const QString &id, bool force)
{
#if QT_CONFIG(webengine_extensions)
    if (id.isEmpty())
        return;
    if (force) {
        m_healthVerdicts.remove(id);
        m_healthDetails.remove(id);
    } else if (m_healthVerdicts.contains(id)) {
        return;
    }
    for (const auto &entry : m_healthQueue) {
        if (entry.first == id)
            return;
    }
    if (m_healthProbeId == id)
        return;
    QWebEngineExtensionManager *engine = managerFor(m_profiles);
    // Only a manifest-declared background can die — skip the page
    // entirely for extensions that have none.
    for (const ExtensionInfo &info : extensions()) {
        if (info.id != id)
            continue;
        if (info.path.isEmpty()
                || !inspectManifest(info.path).hasBackground) {
            m_healthVerdicts.insert(id, ProbeVerdict::None);
            emit changed();
            return;
        }
        break;
    }
    for (const QPointer<QWebEngineProfile> &profile : m_profiles) {
        if (profile && profile->extensionManager() == engine) {
            m_healthQueue.append(qMakePair(id, profile));
            break;
        }
    }
    startNextHealthProbe();
#else
    Q_UNUSED(id);
    Q_UNUSED(force);
#endif
}

void ExtensionManager::startNextHealthProbe()
{
#if QT_CONFIG(webengine_extensions)
    if (m_healthPage || m_healthQueue.isEmpty())
        return;
    const auto request = m_healthQueue.takeFirst();
    QWebEngineProfile *profile = request.second;
    if (!profile) {
        startNextHealthProbe();
        return;
    }
    // Re-resolve the row — the extension may have been disabled or
    // removed since it queued.
    ExtensionInfo info;
    for (const ExtensionInfo &candidate : extensions()) {
        if (candidate.id == request.first) {
            info = candidate;
            break;
        }
    }
    if (!info.loaded || !info.enabled) {
        startNextHealthProbe();
        return;
    }

    m_healthProbeId = info.id;
    m_healthProfile = profile;
    m_healthPopupStage = false;
    m_healthVerdicts.insert(info.id, ProbeVerdict::Pending);

    // A bare page on the extension's profile — deliberately not the
    // app's WebPage subclass, same as the EXT08 popup host: no arora
    // channel objects or page policy leak into extension chrome.
    QWebEnginePage *page = new QWebEnginePage(profile);
    m_healthPage = page;
    connect(page, &QWebEnginePage::loadFinished,
            this, [this](bool ok) {
        if (!m_healthPage)
            return;
        if (ok) {
            runHealthProbeScript();
            return;
        }
        // The manifest.json document failed — try the declared popup
        // page once before giving up on this extension.
        ExtensionInfo info;
        for (const ExtensionInfo &candidate : extensions()) {
            if (candidate.id == m_healthProbeId)
                info = candidate;
        }
        if (!m_healthPopupStage && info.actionPopupUrl.isValid()) {
            m_healthPopupStage = true;
            m_healthPage->load(info.actionPopupUrl);
            return;
        }
        finishHealthProbe(ProbeVerdict::None,
            tr("could not load a probe page for this extension"));
    });
    connect(profile, &QObject::destroyed, this,
            [this]() { abandonHealthProbe(); });
    page->load(QUrl(QStringLiteral("chrome-extension://%1/manifest.json")
                        .arg(info.id)));
    // Watchdog: a wedged load or a dropped eval must not stall the
    // queue — an unmeasurable extension stays static-classified.
    QTimer::singleShot(30000, this, [this, page]() {
        if (m_healthPage == page)
            finishHealthProbe(ProbeVerdict::None,
                tr("health probe timed out"));
    });
#endif
}

void ExtensionManager::runHealthProbeScript()
{
#if QT_CONFIG(webengine_extensions)
    QWebEnginePage *page = m_healthPage;
    if (!page)
        return;
    page->runJavaScript(QString::fromUtf8(kHealthProbeScript),
                        [](const QVariant &) {});
    // Poll for the report — runJavaScript does not await the
    // promise-shaped probe, so the script parks its JSON on window.
    QTimer *poll = new QTimer(page);
    poll->setInterval(400);
    const auto tries = std::make_shared<int>(0);
    connect(poll, &QTimer::timeout, page, [this, poll, page, tries]() {
        if (m_healthPage != page) {
            poll->stop();
            poll->deleteLater();
            return;
        }
        ++*tries;
        page->runJavaScript(
            QStringLiteral("window.__aroraExtHealth || ''"),
            [this, poll, tries](const QVariant &result) {
                const QByteArray json = result.toString().toUtf8();
                if (json.isEmpty() && *tries <= 30)
                    return;
                poll->stop();
                poll->deleteLater();
                evaluateHealthReport(json);
            });
    });
    poll->start();
#endif
}

void ExtensionManager::evaluateHealthReport(const QByteArray &json)
{
#if QT_CONFIG(webengine_extensions)
    const QJsonObject report = QJsonDocument::fromJson(json).object();
    if (!report.value(QLatin1String("runtime")).toBool()) {
        // manifest.json got no chrome.* bindings — retry once on the
        // declared action popup, which is a guaranteed extension
        // context (EXT08 probes it the same way).
        ExtensionInfo info;
        for (const ExtensionInfo &candidate : extensions()) {
            if (candidate.id == m_healthProbeId)
                info = candidate;
        }
        if (!m_healthPopupStage && info.actionPopupUrl.isValid()
                && m_healthPage) {
            m_healthPopupStage = true;
            m_healthPage->load(info.actionPopupUrl);
            return;
        }
        finishHealthProbe(ProbeVerdict::None,
            tr("the probe page got no chrome.* bindings"));
        return;
    }

    const QString ping = report.value(QLatin1String("ping")).toString();
    const int backgrounds =
        report.value(QLatin1String("backgrounds")).toInt(-2);
    if (ping == QLatin1String("TIMEOUT")) {
        // EXT11's Bitwarden signature: the sendMessage triggered a
        // worker start that never became ready (eval crash → status
        // 15 → Chromium retries forever).
        finishHealthProbe(ProbeVerdict::Dead,
            tr("the background ping timed out — its service worker "
               "never became ready"));
        return;
    }
    // ERR with zero BACKGROUND contexts means the ping returned
    // without a worker ever registering — but only when the manifest
    // actually declares one (an extension with no background rightly
    // shows none, and a response that did arrive is itself proof of
    // a live context, so the check is gated on the error path).
    bool declaresBackground = false;
    for (const ExtensionInfo &candidate : extensions()) {
        if (candidate.id == m_healthProbeId
                && !candidate.path.isEmpty()) {
            declaresBackground =
                inspectManifest(candidate.path).hasBackground;
            break;
        }
    }
    if (declaresBackground && backgrounds == 0
            && ping.startsWith(QLatin1String("ERR"))) {
        finishHealthProbe(ProbeVerdict::Dead,
            tr("its background service worker failed to register"));
        return;
    }
    // PONG / EMPTY / "port closed" / a fast connection error with a
    // live BACKGROUND context — the worker evaluated and stays
    // registered, whether or not it answers foreign pings.
    finishHealthProbe(ProbeVerdict::Responsive,
        QStringLiteral("ping=%1 backgrounds=%2")
            .arg(ping).arg(backgrounds));
#endif
}

void ExtensionManager::finishHealthProbe(ProbeVerdict verdict,
                                         const QString &detail)
{
    const QString id = m_healthProbeId;
    m_healthProbeId.clear();
    delete m_healthPage;
    m_healthPage = nullptr;
    m_healthProfile = nullptr;
    if (!id.isEmpty()) {
        if (verdict != ProbeVerdict::Pending)
            m_healthVerdicts.insert(id, verdict);
        else
            m_healthVerdicts.remove(id);
        m_healthDetails.insert(id, detail);
        emit changed();
    }
    // Drain the queue — one probe page at a time.
    QTimer::singleShot(0, this, [this]() { startNextHealthProbe(); });
}

QString ExtensionManager::healthProbeDetail(const QString &id) const
{
    return m_healthDetails.value(id);
}

void ExtensionManager::abandonHealthProbe()
{
    // The profile under the probe page is going away — drop the page
    // first, then leave the id verdict-less so the next attach can
    // measure fresh.
    const QString id = m_healthProbeId;
    m_healthProbeId.clear();
    delete m_healthPage;
    m_healthPage = nullptr;
    m_healthProfile = nullptr;
    if (!id.isEmpty())
        m_healthVerdicts.remove(id);
    m_healthQueue.clear();
}

QString ExtensionManager::userScriptsPath()
{
    const QString directory =
        BrowserPaths::dataFilePath(QLatin1String("userscripts"));
    QDir().mkpath(directory);
    // EXT05: every file here executes in each page's ApplicationWorld
    // — the store must stay owner-only so another account cannot
    // plant a script or read what the user runs.
    QFile::setPermissions(directory,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner
        | QFileDevice::ExeOwner);
    return directory;
}

QStringList ExtensionManager::userScriptNames() const
{
    return QDir(userScriptsPath(), QLatin1String("*.js"),
              QDir::Name, QDir::Files).entryList();
}

void ExtensionManager::reloadUserScripts()
{
    for (const QPointer<QWebEngineProfile> &profile : m_profiles) {
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
        const QString fullPath = dir.absoluteFilePath(fileName);
        // EXT05: a group/world-accessible userscript means another
        // account could have planted it — tighten it and say so.
        const QFileDevice::Permissions leaked =
            QFileInfo(fullPath).permissions()
            & (QFileDevice::ReadGroup | QFileDevice::WriteGroup
               | QFileDevice::ExeGroup | QFileDevice::ReadOther
               | QFileDevice::WriteOther | QFileDevice::ExeOther);
        if (leaked) {
            qWarning() << "userscript" << fileName
                       << "was group/world accessible — permissions"
                          " tightened to owner-only";
            QFile::setPermissions(fullPath,
                QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        }
        QFile file(fullPath);
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
// dotted-quad version and download the declared package.  EXT05:
// downloads are https-only (loopback http allowed for fixtures) and
// a check never installs — a verified .zip is flagged "installable"
// and the UI applies it through the same consent review as a manual
// install, so changed or dangerous permissions are re-acknowledged.
// A package that carries the manifest "key" lands under the same
// extension id and replaces the old version.  .crx packages cannot
// be installed by Qt at all — they are saved under updatesPath()
// for manual handling.
//
// EXT06: under CONFIG+=rustcore the downloaded bytes are verified by
// rc_ext_verify_package BEFORE they hit disk — the update manifest's
// declared hash_sha256 (when present), the zip/CRX3 structure and
// member names, and an embedded readable manifest.json are all
// required.  Rejected packages are never written.

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
        QString hash;   // hash_sha256 — declared package digest (hex)
    };
    QList<Offer> offers;
#if defined(ARORA_RUSTCORE)
    // SEC19: remote XML is parsed by memory-safe Rust — the crate
    // hands back {"offers":[[appid,status,codebase,version,
    // hash_sha256],...]} and every policy decision below stays
    // exactly where it was.
    RcBuffer out{};
    const RcStatus parsed = rc_updatemanifest_parse(
        reinterpret_cast<const uint8_t *>(xml.constData()),
        size_t(xml.size()), &out);
    if (parsed != RC_OK) {
        char *message = rc_last_error_message();
        result->error = tr("update manifest is not valid XML: %1")
            .arg(message ? QString::fromUtf8(message)
                         : tr("malformed document"));
        rc_string_free(message);
        return false;
    }
    const QByteArray json(reinterpret_cast<const char *>(out.data),
                          qsizetype(out.len));
    rc_buffer_free(out);
    const QJsonArray rows = QJsonDocument::fromJson(json)
        .object().value(QLatin1String("offers")).toArray();
    for (const QJsonValue &row : rows) {
        const QJsonArray fields = row.toArray();
        if (fields.size() < 4)
            continue;
        Offer offer;
        offer.appId = fields.at(0).toString();
        offer.status = fields.at(1).toString();
        offer.codebase = fields.at(2).toString();
        offer.version = fields.at(3).toString();
        if (fields.size() > 4)
            offer.hash = fields.at(4).toString();
        offers.append(offer);
    }
#else
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
                offer.hash = attrs.value(QLatin1String("hash_sha256")).toString();
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
#endif // ARORA_RUSTCORE
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
    if (!isFetchableUpdateUrl(codebase)) {
        // EXT05: update payloads travel over https — a remote http
        // URL is refused here and again at fetch time.
        result->error = tr("update package URL \"%1\" is not https")
            .arg(offer->codebase);
        return false;
    }
    result->availableVersion = offer->version;
    result->codeBase = codebase;
    result->packageHash = offer->hash;
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
        const QWebEngineExtensionInfo info = findById(extensions, id);
        if (!info.id().isEmpty() && !info.path().isEmpty()) {
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
    m_updateResults.clear();
    emit updateCheckStarted();
    // EXT05: manual and automatic checks behave identically inside
    // the manager — neither installs — so the manual flag no longer
    // gates anything here; apply is always the UI's opt-in call.
    Q_UNUSED(manual);

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
        // EXT05: refuse non-https update endpoints at fetch time —
        // loopback http stays allowed for local fixtures.
        if (updateUrl.isEmpty() || !isFetchableUpdateUrl(url)) {
            result.noSource = true;
            if (!updateUrl.isEmpty())
                result.error = tr("update_url \"%1\" is not https")
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

    // EXT05 fetch-time gate — a URL that slipped past the caller's
    // check is refused here rather than fetched.  No finishUpdateCheck
    // — the only caller is checkForUpdates' loop, which finishes once.
    if (!isFetchableUpdateUrl(updateUrl)) {
        job.result.error = tr("update_url \"%1\" is not https")
            .arg(updateUrl.toString());
        m_updateResults.append(job.result);
        m_lastUpdateResults.insert(job.result.id, job.result);
        return;
    }

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

    // EXT05 fetch-time gate — the codebase was validated at parse,
    // but nothing reaches the network without passing it again.
    if (!isFetchableUpdateUrl(result.codeBase)) {
        job.result.error = tr("update package URL \"%1\" is not https")
            .arg(result.codeBase.toString());
        m_updateResults.append(job.result);
        m_lastUpdateResults.insert(job.result.id, job.result);
        finishUpdateCheck();
        return;
    }

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
            const QByteArray expected =
                QByteArray::fromHex(job.result.packageHash.toLatin1());
#if defined(ARORA_RUSTCORE)
            // EXT06: verify the fetched bytes before they hit disk —
            // the update manifest's declared hash (when it carries
            // one), the zip/CRX3 structure, member names and a
            // readable manifest.json are all required.  A rejected
            // package is never written, let alone installed.
            RcBuffer verdictBuf{};
            const RcStatus verified = rc_ext_verify_package(
                reinterpret_cast<const uint8_t *>(body.constData()),
                size_t(body.size()),
                expected.size() == 32
                    ? reinterpret_cast<const uint8_t *>(expected.constData())
                    : nullptr,
                nullptr, 0, &verdictBuf);
            QString verifyError;
            if (verified == RC_OK) {
                const QJsonObject verdict = QJsonDocument::fromJson(
                    QByteArray(reinterpret_cast<const char *>(verdictBuf.data),
                               qsizetype(verdictBuf.len))).object();
                rc_buffer_free(verdictBuf);
                if (verdict.value(QLatin1String("status")).toString()
                        != QLatin1String("valid")) {
                    verifyError = tr("update package rejected: %1")
                        .arg(verdict.value(QLatin1String("error")).toString());
                } else {
                    const QJsonObject check =
                        verdict.value(QLatin1String("manifest")).toObject();
                    if (check.isEmpty()
                        || !check.value(QLatin1String("valid")).toBool())
                        verifyError = tr("update package carries no readable "
                                         "manifest.json");
                }
            } else {
                char *message = rc_last_error_message();
                verifyError = tr("update package verification failed: %1")
                    .arg(message ? QString::fromUtf8(message)
                                 : tr("error"));
                rc_string_free(message);
            }
            if (!verifyError.isEmpty()) {
                job.result.error = verifyError;
                m_updateResults.append(job.result);
                m_lastUpdateResults.insert(job.result.id, job.result);
                finishUpdateCheck();
                return;
            }
#else
            // EXT05: no-Rust builds still honor the update manifest's
            // declared hash_sha256 — a tampered payload is refused
            // before it hits disk.  (Structure checks are rustcore's;
            // without it the hash is the only integrity gate.)
            if (expected.size() == 32
                && QCryptographicHash::hash(body,
                        QCryptographicHash::Sha256) != expected) {
                job.result.error =
                    tr("update package rejected: sha256 does not match "
                       "the update manifest's declared hash");
                m_updateResults.append(job.result);
                m_lastUpdateResults.insert(job.result.id, job.result);
                finishUpdateCheck();
                return;
            }
#endif
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
                // EXT05: the check NEVER installs — apply is opt-in,
                // the UI reviews the package like a manual install
                // and grants consent per path first.
                job.result.installable = body.startsWith("PK\x03\x04")
                    || fileName.endsWith(QLatin1String(".zip"),
                                         Qt::CaseInsensitive);
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
