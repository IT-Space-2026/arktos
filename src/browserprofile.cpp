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

#include "browserprofile.h"

#include "acceptlanguagedialog.h"
#include "privacyrequestinterceptor.h"

#include <qapplication.h>
#include <qdir.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qregularexpression.h>
#include <qset.h>
#include <qsettings.h>
#include <qwebengineclienthints.h>
#include <qwebengineprofile.h>
#include <qwebenginescript.h>
#include <qwebenginescriptcollection.h>
#include <qwebenginesettings.h>

#include <ctime>

namespace BrowserProfile {

QWebEngineProfile *normalProfile()
{
    // Lazily-created so callers do not need to coordinate with main():
    // every user (main.cpp, CookieJar::instance(), the settings dialog)
    // gets the same named "arora" profile.
    static QWebEngineProfile *profile = nullptr;
    if (!profile) {
        profile = new QWebEngineProfile(QLatin1String("arora"), qApp);
        // HARD01: a previous session's site-data clear may have
        // deferred its service-coupled storage trees to this moment —
        // the profile exists but no page has ever used it, so the
        // directories can be removed without wedging the storage
        // services (see clearSiteStorage).
        clearDeferredSiteStorage(profile->persistentStoragePath());
    }
    return profile;
}

static QWebEngineProfile *s_privateProfile = nullptr;

QWebEngineProfile *privateProfile()
{
    // An unnamed profile is off-the-record: nothing hits disk.
    if (!s_privateProfile)
        s_privateProfile = new QWebEngineProfile(qApp);
    return s_privateProfile;
}

QWebEngineProfile *privateProfileIfCreated()
{
    return s_privateProfile;
}

static QWebEngineProfile *s_torProfile = nullptr;

QWebEngineProfile *torProfile()
{
    // An unnamed profile is off-the-record: nothing hits disk.
    if (!s_torProfile) {
        s_torProfile = new QWebEngineProfile(qApp);
        s_torProfile->setHttpCacheType(QWebEngineProfile::MemoryHttpCache);
    }
    return s_torProfile;
}

QString defaultHttpUserAgent()
{
    // UA01: Qt's factory UA carries a "QtWebEngine/<ver>" product
    // token that bot-detection fingerprints as automation — Google's
    // /sorry/ interstitial fired on a plain google.com search.  Vanilla
    // Chrome strings do not trip it, so Arora ships Qt's own default
    // minus that token; the platform and bundled-Chromium version
    // tokens stay accurate.  A throwaway anonymous profile is asked
    // because the browsing profiles cannot be — applySettings() may
    // already have overridden their UA by the time this runs.
    static const QString userAgent = [] {
        QWebEngineProfile probe;
        QString ua = probe.httpUserAgent();
        ua.remove(QRegularExpression(QLatin1String("\\s*QtWebEngine/\\S+")));
        return ua;
    }();
    return userAgent;
}

void applyClientHints(QWebEngineProfile *profile)
{
    if (!profile)
        return;
    QWebEngineClientHints *hints = profile->clientHints();
    if (!profile->httpUserAgent().contains(QLatin1String("Chrome/"))) {
        hints->resetAll();
        return;
    }
    // The brand list maps name -> full version ("Chromium" ->
    // "140.0.7339.225"); the low-entropy Sec-CH-UA major-version list
    // and the greased brand are derived from it automatically.
    QVariantMap brands = hints->fullVersionList();
    const QString chromiumVersion =
        brands.value(QLatin1String("Chromium")).toString();
    if (chromiumVersion.isEmpty()
        || brands.value(QLatin1String("Google Chrome")).toString()
            == chromiumVersion)
        return;
    brands.insert(QLatin1String("Google Chrome"), chromiumVersion);
    hints->setFullVersionList(brands);
}

// QWebEngineScript has no "replace" — remove a previously installed
// user style sheet by name before inserting the new one.
static void installUserStyleSheet(QWebEngineProfile *profile, const QUrl &url)
{
    const QString name = QLatin1String("aroraUserStyleSheet");
    QWebEngineScriptCollection *scripts = profile->scripts();
    const QList<QWebEngineScript> installed = scripts->toList();
    for (const QWebEngineScript &script : installed) {
        if (script.name() == name)
            scripts->remove(script);
    }
    if (url.isEmpty())
        return;

    QString css;
    if (url.isLocalFile()) {
        QFile file(url.toLocalFile());
        if (file.open(QIODevice::ReadOnly))
            css = QString::fromUtf8(file.readAll());
    }

    // Escape a string for embedding as a JS string literal.
    const auto jsQuote = [](QString text) {
        text.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
        text.replace(QLatin1Char('"'), QLatin1String("\\\""));
        text.replace(QLatin1Char('\n'), QLatin1String("\\n"));
        text.remove(QLatin1Char('\r'));
        return text;
    };

    QString source;
    if (!css.isEmpty()) {
        source = QStringLiteral(
            "(function(){"
            "var style=document.createElement('style');"
            "style.type='text/css';"
            "style.appendChild(document.createTextNode(\"%1\"));"
            "document.documentElement.appendChild(style);})();")
            .arg(jsQuote(css));
    } else if (url.scheme() == QLatin1String("http")
               || url.scheme() == QLatin1String("https")) {
        // Remote css cannot be read synchronously; link it instead.
        source = QStringLiteral(
            "(function(){"
            "var link=document.createElement('link');"
            "link.rel='stylesheet';link.type='text/css';link.href=\"%1\";"
            "document.documentElement.appendChild(link);})();")
            .arg(jsQuote(QString::fromUtf8(url.toEncoded())));
    }
    if (source.isEmpty())
        return;

    QWebEngineScript script;
    script.setName(name);
    script.setInjectionPoint(QWebEngineScript::DocumentReady);
    script.setRunsOnSubFrames(true);
    // A separate world keeps page scripts from tripping over the
    // injection; the DOM is shared so the style still applies.
    script.setWorldId(QWebEngineScript::ApplicationWorld);
    script.setSourceCode(source);
    scripts->insert(script);
}

void applySettings(QWebEngineProfile *profile)
{
    QWebEngineSettings *engineSettings = profile->settings();

    QSettings settings;
    settings.beginGroup(QLatin1String("websettings"));

    QFont standardFont(engineSettings->fontFamily(QWebEngineSettings::StandardFont),
                       engineSettings->fontSize(QWebEngineSettings::DefaultFontSize));
    standardFont = settings.value(QLatin1String("standardFont"), standardFont).value<QFont>();
    engineSettings->setFontFamily(QWebEngineSettings::StandardFont, standardFont.family());
    engineSettings->setFontSize(QWebEngineSettings::DefaultFontSize, standardFont.pointSize());
    int minimumFontSize = settings.value(QLatin1String("minimumFontSize"),
                                         engineSettings->fontSize(QWebEngineSettings::MinimumFontSize)).toInt();
    engineSettings->setFontSize(QWebEngineSettings::MinimumFontSize, minimumFontSize);

    QFont fixedFont(engineSettings->fontFamily(QWebEngineSettings::FixedFont),
                    engineSettings->fontSize(QWebEngineSettings::DefaultFixedFontSize));
    fixedFont = settings.value(QLatin1String("fixedFont"), fixedFont).value<QFont>();
    engineSettings->setFontFamily(QWebEngineSettings::FixedFont, fixedFont.family());
    engineSettings->setFontSize(QWebEngineSettings::DefaultFixedFontSize, fixedFont.pointSize());

    engineSettings->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows,
                                 !settings.value(QLatin1String("blockPopupWindows"), true).toBool());
    engineSettings->setAttribute(QWebEngineSettings::JavascriptEnabled,
                                 settings.value(QLatin1String("enableJavascript"), true).toBool());
    engineSettings->setAttribute(QWebEngineSettings::PluginsEnabled,
                                 settings.value(QLatin1String("enablePlugins"), true).toBool());
    engineSettings->setAttribute(QWebEngineSettings::AutoLoadImages,
                                 settings.value(QLatin1String("enableImages"), true).toBool());
    engineSettings->setAttribute(QWebEngineSettings::LocalStorageEnabled,
                                 settings.value(QLatin1String("enableLocalStorage"), true).toBool());
    engineSettings->setAttribute(QWebEngineSettings::DnsPrefetchEnabled, true);

    // SECLVL: Mullvad-style security tiers (privacy/securityLevel).
    // Safer and Safest force click-to-play media; Safest additionally
    // disables JavaScript profile-wide — applied after the
    // enableJavascript toggle above so the tier wins.  The tier's
    // other half (dropping external script fetches on plain-http
    // pages from Safer up) is the IO-thread snapshot refreshed by
    // PrivacyRequestInterceptor::loadSettings() below.  Composition
    // with the SEC05 broker needs nothing extra: capture/clipboard/
    // screen stay hard-denied under every tier and AskEveryTime keeps
    // Chromium's own store out of the loop.
    const int securityLevel = qBound(
        int(PrivacyRequestInterceptor::Standard),
        QSettings().value(QLatin1String("privacy/securityLevel"),
                          int(PrivacyRequestInterceptor::Standard)).toInt(),
        int(PrivacyRequestInterceptor::Safest));
    engineSettings->setAttribute(
        QWebEngineSettings::PlaybackRequiresUserGesture,
        securityLevel >= int(PrivacyRequestInterceptor::Safer));
    if (securityLevel >= int(PrivacyRequestInterceptor::Safest))
        engineSettings->setAttribute(QWebEngineSettings::JavascriptEnabled, false);

    // SEC05 surface audit: attributes that would let a page sidestep
    // the WebPermissionManager broker stay off.  Several are already
    // off by default in Qt 6.11 — they are pinned anyway so a future
    // Qt default flip or a stale settings file cannot re-open them.
    //   JavascriptCanAccessClipboard / JavascriptCanPaste
    //     silent clipboard reads/writes, bypassing ClipboardReadWrite
    //   ScreenCaptureEnabled          getDisplayMedia without consent
    //   LocalContentCanAccessRemoteUrls   file:// pages phoning home
    //   AllowRunningInsecureContent   http subresources on https pages
    //   AllowGeolocationOnInsecureOrigins location on plain http
    // WebRTCPublicInterfacesOnly keeps WebRTC peer connections off
    // LAN/private addresses (media capture is denied anyway).
    engineSettings->setAttribute(QWebEngineSettings::JavascriptCanAccessClipboard, false);
    engineSettings->setAttribute(QWebEngineSettings::JavascriptCanPaste, false);
    engineSettings->setAttribute(QWebEngineSettings::ScreenCaptureEnabled, false);
    engineSettings->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
    engineSettings->setAttribute(QWebEngineSettings::AllowRunningInsecureContent, false);
    engineSettings->setAttribute(QWebEngineSettings::AllowGeolocationOnInsecureOrigins, false);
    // PRIV01: the WebRTC leak protection is a user toggle (default
    // on); applyChromiumFlags() couples the same setting to the
    // stronger Chromium IP-handling policy at process level.
    engineSettings->setAttribute(QWebEngineSettings::WebRTCPublicInterfacesOnly,
        QSettings().value(QLatin1String("privacy/webrtcIpProtection"), true).toBool());

    // PRIV01: refresh the request interceptor's IO-thread snapshot
    // (https-first upgrade, referrer trim) alongside the profile
    // settings — the privacy group lives outside websettings/network.
    PrivacyRequestInterceptor::loadSettings();

    // Named profiles default to StoreOnDisk: Chromium would write every
    // grant()/deny() to permissions.json on disk and silently apply it
    // on later visits without even emitting permissionRequested —
    // bypassing the WebPermissionManager broker, its "Remember this
    // decision" checkbox and its auditable store.  AskEveryTime keeps
    // the engine stateless so the broker is the only authority.
    profile->setPersistentPermissionsPolicy(
        QWebEngineProfile::PersistentPermissionsPolicy::AskEveryTime);
    // Deliberately NOT pinned: LocalContentCanAccessFileUrls stays at
    // Qt's default (on) or local HTML files could not reference sibling
    // images/css; FullScreenSupportEnabled stays off (Qt default)
    // because there is no fullScreenRequested handler wired to the
    // window yet.

    installUserStyleSheet(profile, settings.value(QLatin1String("userStyleSheet")).toUrl());
    settings.endGroup();

    // The app's network cache preference drives both the profile's
    // Chromium cache and (through NetworkAccessManager::loadSettings)
    // the app-side fetch cache.
    settings.beginGroup(QLatin1String("network"));
    bool cacheEnabled = settings.value(QLatin1String("cacheEnabled"), true).toBool();
    profile->setHttpCacheType(cacheEnabled ? QWebEngineProfile::DiskHttpCache
                                           : QWebEngineProfile::NoCache);
    profile->setHttpCacheMaximumSize(
        settings.value(QLatin1String("maximumCacheSize"), 50).toInt() * 1024 * 1024);
    profile->setHttpAcceptLanguage(
        QString::fromUtf8(AcceptLanguageDialog::httpString(AcceptLanguageDialog::acceptLanguages())));
    settings.endGroup();

    // UA01: the UserAgentMenu override lives in a top-level key.  When
    // unset, send the de-badged vanilla UA rather than Qt's factory
    // default — the "QtWebEngine/<ver>" token trips Google's bot check.
    const QString userAgent =
        settings.value(QLatin1String("userAgent")).toString();
    profile->setHttpUserAgent(
        userAgent.isEmpty() ? defaultHttpUserAgent() : userAgent);
    applyClientHints(profile);

    // SEC12: the profile tree must stay owner-only.  This runs at
    // startup so a storage dir loosened by a umask quirk or a manual
    // copy is repaired before Chromium writes more into it.  PERF01:
    // the deep walk runs once per profile per process — applySettings
    // is called again by postLaunch's loadSettings and by every
    // settings-dialog apply, and the recursive stat+chmod pass over a
    // warm Chromium cache tree is too expensive to repeat.  The
    // post-wipe calls in clearSiteStorage/clearDeferredSiteStorage go
    // through ensureUserOnlyPermissions directly and still run.
    if (!profile->isOffTheRecord()) {
        static QSet<QString> checkedPaths;
        const QString storagePath = profile->persistentStoragePath();
        if (!checkedPaths.contains(storagePath)) {
            checkedPaths.insert(storagePath);
            ensureUserOnlyPermissions(storagePath);
        }
    }
}

bool ensureUserOnlyPermissions(const QString &path)
{
    const QFile::Permissions dirPerms =
        QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
    const QFile::Permissions filePerms =
        QFile::ReadOwner | QFile::WriteOwner;

    QDir dir(path);
    if (!dir.exists())
        return true;

    // entryInfoList already stats every entry — skip the chmod call
    // unless the permissions actually differ, so a correctly-formed
    // tree costs one read-only pass instead of a write syscall per
    // file (PERF01: this walks the whole Chromium profile incl. the
    // HTTP cache on the startup path).
    bool ok = true;
    if (QFileInfo(dir.absolutePath()).permissions() != dirPerms)
        ok = QFile::setPermissions(dir.absolutePath(), dirPerms);
    const QFileInfoList entries = dir.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const QFileInfo &entry : entries) {
        // Never chmod through a symlink — it could point outside the
        // profile tree.
        if (entry.isSymLink())
            continue;
        if (entry.isDir())
            ok &= ensureUserOnlyPermissions(entry.absoluteFilePath());
        else if (entry.permissions() != filePerms)
            ok &= QFile::setPermissions(entry.absoluteFilePath(), filePerms);
    }
    return ok;
}

// The whole site-data storage class is service-lifecycle-coupled:
// HARD01 bisected "Local Storage", "IndexedDB", "Service Worker" and
// "WebStorage" removals each wedging the profile outright — the next
// navigation hangs because Chromium opens and holds these trees
// inside its storage services.  Nothing in this class is ever
// removed under a running browser; clearDeferredSiteStorage() wipes
// them at the next profile startup, before any WebContents exists.
static const char *const deferredSiteDirs[] = {
    "Local Storage",        // localStorage leveldb
    "Session Storage",      // sessionStorage leveldb
    "IndexedDB",
    "Service Worker",       // registrations + script cache
    "databases",            // WebSQL
    "File System",          // FileSystem API / OPFS buckets
    "blob_storage",         // blob registry
    "WebStorage",           // QuotaManager bookkeeping
    "Shared Dictionary",
    "Platform Notifications",
    "InterestGroups",
    "PrivateAggregation",
    "AttributionReporting",
};

static const char *const deferredSiteFiles[] = {
    "Trust Tokens",
    "Trust Tokens-journal",
    "DIPS",
    "DIPS-journal",
    "Network Persistent State",     // persisted HSTS state
};

// Directories additionally removed by the exit-wipe sentinel
// (clearAllStorageOnNextStart): Chromium's cookie database, HSTS and
// reporting state live under Network/, the http cache under Cache/
// and Code Cache/.  Dropped wholesale rather than file-by-file
// because an exiting process cannot wait for the network service's
// async deletes to flush.
static const char *const exitWipeDirs[] = {
    "Network",
    "Cache",
    "Code Cache",
};

static QString deferredWipeSentinel(const QString &storagePath)
{
    return storagePath + QLatin1String("/arora-site-wipe.pending");
}

static QString exitWipeSentinel(const QString &storagePath)
{
    return storagePath + QLatin1String("/arora-exit-wipe.pending");
}

bool clearSiteStorage(QWebEngineProfile *profile)
{
    // Off-the-record profiles keep everything in memory — nothing to
    // remove — and their storage path may be empty.
    if (!profile || profile->isOffTheRecord())
        return true;

    const QString storagePath = profile->persistentStoragePath();
    if (storagePath.isEmpty())
        return true;

    // HARD01: nothing is deleted under the running browser (see
    // above) — the page-side script sweep in ClearPrivateData empties
    // open origins in memory, and the on-disk trees go away at the
    // next profile start.  Drop the sentinel that schedules it.
    bool ok = true;
    QFile sentinel(deferredWipeSentinel(storagePath));
    if (!sentinel.open(QIODevice::WriteOnly))
        ok = false;
    else
        sentinel.close();

    // Whatever survived or was recreated still sits inside the
    // private-data tree — keep it owner-only.
    ensureUserOnlyPermissions(storagePath);
    return ok;
}

bool clearAllStorageOnNextStart(QWebEngineProfile *profile)
{
    // Same discipline as clearSiteStorage: nothing is removed while
    // the browser runs — both sentinels are dropped and the next
    // profile start does the deleting.
    if (!profile || profile->isOffTheRecord())
        return true;
    const QString storagePath = profile->persistentStoragePath();
    if (storagePath.isEmpty())
        return true;

    bool ok = clearSiteStorage(profile);
    QFile sentinel(exitWipeSentinel(storagePath));
    if (!sentinel.open(QIODevice::WriteOnly))
        ok = false;
    else
        sentinel.close();
    return ok;
}

bool clearDeferredSiteStorage(const QString &storagePath)
{
    if (storagePath.isEmpty())
        return true;
    const QString sentinelPath = deferredWipeSentinel(storagePath);
    const QString exitSentinelPath = exitWipeSentinel(storagePath);
    const bool siteWipe = QFile::exists(sentinelPath);
    const bool exitWipe = QFile::exists(exitSentinelPath);
    if (!siteWipe && !exitWipe)
        return true;

    bool ok = true;
    for (const char *name : deferredSiteDirs) {
        QDir dir(storagePath + QLatin1Char('/') + QLatin1String(name));
        if (dir.exists())
            ok &= dir.removeRecursively();
    }
    for (const char *name : deferredSiteFiles) {
        QFile file(storagePath + QLatin1Char('/') + QLatin1String(name));
        if (file.exists())
            ok &= file.remove();
    }
    if (exitWipe) {
        for (const char *name : exitWipeDirs) {
            QDir dir(storagePath + QLatin1Char('/') + QLatin1String(name));
            if (dir.exists())
                ok &= dir.removeRecursively();
        }
    }
    if (siteWipe)
        ok &= QFile::remove(sentinelPath);
    if (exitWipe)
        ok &= QFile::remove(exitSentinelPath);
    ensureUserOnlyPermissions(storagePath);
    return ok;
}

void applyChromiumFlags()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("privacy"));
    const bool webrtcProtection =
        settings.value(QLatin1String("webrtcIpProtection"), true).toBool();
    const bool secureDns =
        settings.value(QLatin1String("secureDns"), false).toBool();
    settings.endGroup();
    if (!webrtcProtection && !secureDns)
        return;

    QStringList flags = QString::fromLocal8Bit(
        qgetenv("QTWEBENGINE_CHROMIUM_FLAGS"))
        .split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const auto addFlag = [&flags](const QString &flag) {
        if (!flags.contains(flag))
            flags.append(flag);
    };
    if (webrtcProtection) {
        // Strongest WebRTC IP policy Chromium exposes: every ICE
        // transport goes through the configured proxy (SOCKS5 cannot
        // forward UDP, so under Tor this simply disables WebRTC —
        // capture devices are already hard-denied by the SEC05
        // broker, so nothing usable is lost).
        addFlag(QLatin1String(
            "--force-webrtc-ip-handling-policy=disable_non_proxied_udp"));
    }
    if (secureDns) {
        // Auto-upgrade mode only — a custom DoH endpoint cannot be
        // expressed through flags (that needs the SecureDnsMode pref,
        // which QtWebEngine does not expose).
        addFlag(QLatin1String("--enable-features=DnsOverHttps"));
    }
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", flags.join(QLatin1Char(' ')).toLocal8Bit());
}

void applyFingerprintEnvironment()
{
    const bool wantUtc = QSettings().value(
        QLatin1String("privacy/reportUtcTimezone"), false).toBool();

    // Remember the caller's own TZ once so toggling the setting off
    // restores exactly what was inherited — a TZ the user exported is
    // never clobbered, and an unset TZ becomes unset again (not
    // empty, which libc parses as UTC on some platforms).
    static bool overridden = false;
    static bool previousWasSet = false;
    static QByteArray previousTz;
    if (wantUtc) {
        if (!overridden) {
            previousWasSet = qEnvironmentVariableIsSet("TZ");
            previousTz = qgetenv("TZ");
            overridden = true;
        }
        qputenv("TZ", "UTC");
    } else if (overridden) {
        if (previousWasSet)
            qputenv("TZ", previousTz);
        else
            qunsetenv("TZ");
        overridden = false;
    }

    // libc caches the zone — the in-process Chromium browser process
    // and Qt's own QTimeZone reads must see the new value.
#if defined(Q_OS_WIN)
    _tzset();
#else
    tzset();
#endif
}

} // namespace BrowserProfile
