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
#include <qdebug.h>
#include <qdir.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qregularexpression.h>
#include <qset.h>
#include <qsettings.h>
#include <qurl.h>
#include <qwebengineclienthints.h>
#include <qwebengineglobalsettings.h>
#include <qwebengineprofile.h>
#include <qwebenginescript.h>
#include <qwebenginescriptcollection.h>
#include <qwebenginesettings.h>

#if defined(ARORA_RUSTCORE)
#include <rustcore.h>
#endif

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

int presentedChromeMajor()
{
    return 155;
}

QString buildHttpUserAgentQt(const QString &factoryUserAgent,
                             const QString &overrideUserAgent)
{
    // The Qt builder — the no-rust implementation, the FFI-failure
    // fallback and the parity-test seam (CPAL01's convention).
    if (!overrideUserAgent.isEmpty())
        return overrideUserAgent;
    QString ua = factoryUserAgent;
    ua.remove(QRegularExpression(QLatin1String("\\s*QtWebEngine/\\S+")));
    ua.replace(QRegularExpression(QLatin1String("Chrome/\\d+")),
               QLatin1String("Chrome/")
                   + QString::number(presentedChromeMajor()));
    return ua;
}

QString buildHttpUserAgent(const QString &factoryUserAgent,
                           const QString &overrideUserAgent)
{
#if defined(ARORA_RUSTCORE)
    // UAG01: the construction decision lives in rustcore — the Qt
    // builder below is byte-identical (the autotest corpus proves
    // it) and serves as the no-rust/FFI-failure path.
    const QJsonObject context{
        {QLatin1String("factory_ua"), factoryUserAgent},
        {QLatin1String("presented_major"), presentedChromeMajor()},
        {QLatin1String("override"), overrideUserAgent},
    };
    const QByteArray json =
        QJsonDocument(context).toJson(QJsonDocument::Compact);
    if (char *out = rc_ua_build(
            reinterpret_cast<const uint8_t *>(json.constData()),
            size_t(json.size()))) {
        const QString ua = QString::fromUtf8(out);
        rc_string_free(out);
        return ua;
    }
#endif
    return buildHttpUserAgentQt(factoryUserAgent, overrideUserAgent);
}

// The engine's own factory UA, probed once: a throwaway anonymous
// profile is asked because the browsing profiles cannot be —
// applySettings() may already have overridden their UA by the time
// this first runs.
static QString factoryHttpUserAgent()
{
    static const QString ua = [] {
        QWebEngineProfile probe;
        return probe.httpUserAgent();
    }();
    return ua;
}

QString effectiveHttpUserAgent(const QString &overrideUserAgent)
{
    return buildHttpUserAgent(factoryHttpUserAgent(), overrideUserAgent);
}

QString defaultHttpUserAgent()
{
    // UA01: Qt's factory UA carries a "QtWebEngine/<ver>" product
    // token that bot-detection fingerprints as automation — Google's
    // /sorry/ interstitial fired on a plain google.com search.  Vanilla
    // Chrome strings do not trip it, so Arora ships Qt's own default
    // minus that token; the platform tokens stay accurate.  UA03: the
    // Chrome/<major> milestone is bumped to presentedChromeMajor() —
    // sites version-sniff it to nag "browser out of date" once the
    // bundled Chromium lags stable.  Only the version digits change;
    // the engine is still the bundled Chromium.
    static const QString userAgent = effectiveHttpUserAgent(QString());
    return userAgent;
}

// The brand version the client hints should carry for an effective UA:
// the UA's own Chrome milestone over the real engine version's build
// tail — "155.0.7339.225" for a "Chrome/155.0.0.0" UA on a Chromium
// "140.0.7339.225" engine — which is the shape real Chrome uses (a
// reduced UA token alongside full-version hints).  Empty when the UA
// does not claim Chrome.
QString presentedBrandVersionQt(const QString &httpUserAgent,
                                const QString &engineVersion)
{
    const QRegularExpressionMatch match =
        QRegularExpression(QLatin1String("Chrome/(\\d+)"))
            .match(httpUserAgent);
    if (!match.hasMatch())
        return QString();
    const int dot = engineVersion.indexOf(QLatin1Char('.'));
    return match.captured(1)
        + (dot > 0 ? engineVersion.mid(dot) : QLatin1String(".0.0.0"));
}

QString presentedBrandVersion(const QString &httpUserAgent,
                              const QString &engineVersion)
{
#if defined(ARORA_RUSTCORE)
    if (char *out = rc_ua_brand_version(
            httpUserAgent.toUtf8().constData(),
            engineVersion.toUtf8().constData())) {
        const QString version = QString::fromUtf8(out);
        rc_string_free(out);
        return version;
    }
#endif
    return presentedBrandVersionQt(httpUserAgent, engineVersion);
}

void applyClientHints(QWebEngineProfile *profile)
{
    if (!profile)
        return;
    QWebEngineClientHints *hints = profile->clientHints();
    QVariantMap brands = hints->fullVersionList();
    const QString presented = presentedBrandVersion(
        profile->httpUserAgent(),
        brands.value(QLatin1String("Chromium")).toString());
    if (presented.isEmpty()) {
        // A non-Chrome UA (e.g. a Firefox preset) gets the honest
        // Chromium defaults.
        hints->resetAll();
        return;
    }
    // The brand list maps name -> full version ("Chromium" ->
    // "140.0.7339.225"); the low-entropy Sec-CH-UA major-version list
    // and the greased brand are derived from it automatically.  Real
    // Chrome brands itself "Chromium" AND "Google Chrome" at the same
    // version — a UA claiming Chrome/155 over hints reporting
    // Chromium/140 is itself a fingerprinting tell, so both brands
    // carry the presented version (the greased "Not*" brand keeps its
    // deliberately unrelated value).
    const QLatin1String chromiumBrand(QLatin1String("Chromium"));
    const QLatin1String chromeBrand(QLatin1String("Google Chrome"));
    bool changed = brands.value(chromiumBrand).toString() != presented;
    brands.insert(chromiumBrand, presented);
    changed |= brands.value(chromeBrand).toString() != presented;
    brands.insert(chromeBrand, presented);
    if (changed)
        hints->setFullVersionList(brands);
    if (hints->fullVersion() != presented)
        hints->setFullVersion(presented);
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

// REF01: pushes the referer level into the document itself.  The
// interceptor's setHttpHeader rewrite never reaches redirect
// follow-up legs — Chromium recomputes the Referer for those from the
// request's stored referrer after the interceptor ran — so the level
// is also expressed as a <meta name="referrer"> injected at
// DocumentCreation: the source document's policy is what the redirect
// chain carries.  Pages that ship their own meta keep theirs (it
// parses after ours and wins); only pages without one get the floor —
// which is also what keeps a site's stricter explicit policy intact.
// The tor profile floors at Trimmed, matching
// applyRefererPolicy's minimumLevel argument.
void installReferrerPolicy(QWebEngineProfile *profile)
{
    const QString name = QLatin1String("aroraReferrerPolicy");
    QWebEngineScriptCollection *scripts = profile->scripts();
    const QList<QWebEngineScript> installed = scripts->toList();
    for (const QWebEngineScript &script : installed) {
        if (script.name() == name)
            scripts->remove(script);
    }

    const int floor = (profile == s_torProfile)
        ? int(PrivacyRequestInterceptor::RefererTrimmed)
        : int(PrivacyRequestInterceptor::RefererEngineDefault);
    const int level = qMax(
        PrivacyRequestInterceptor::storedRefererPolicy(), floor);
    const QByteArray meta =
        PrivacyRequestInterceptor::referrerMetaValue(level);
    if (meta.isEmpty())
        return;

    // At DocumentCreation the parser may not have produced a <head>
    // (or even <html>) yet — attach to the head/root if it exists,
    // else on the first observed insertion.  NEVER append to the
    // document itself: before <html> exists that makes <meta> a
    // document element child and the parser's own root insertion
    // collides with it.  Either way the meta exists before the parser
    // can have queued any subresource, so navigations, subresources
    // and redirect hops are all computed under it.
    const QString source = QStringLiteral(
        "(function(){"
        "var meta=document.createElement('meta');"
        "meta.name='referrer';meta.content='%1';"
        "function attach(){"
        "var p=document.head||document.documentElement;"
        "if(p){p.appendChild(meta);return true}return false}"
        "if(!attach()){"
        "var o=new MutationObserver(function(){if(attach())o.disconnect()});"
        "o.observe(document,{childList:true,subtree:true})}})();")
        .arg(QLatin1String(meta));

    QWebEngineScript script;
    script.setName(name);
    script.setInjectionPoint(QWebEngineScript::DocumentCreation);
    script.setRunsOnSubFrames(true);
    // ApplicationWorld so page scripts cannot remove or rewrite the
    // meta — the DOM is shared, so the policy still applies.
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

    // POPUP01: the attribute stays bound to the blocker preference.
    // false makes Chromium drop zero-user-gesture window.open calls of
    // EVERY shape inside the renderer — including plain opens that
    // would reach WebPage::createWindow tab-typed and slip through as
    // pop-unders.  Gesture-initiated calls still reach createWindow
    // either way; PopupBlocker gates the pop-up-shaped ones there.
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

    // POL03: Chromium's auto-dark for web contents — pages that ship
    // their own dark scheme (prefers-color-scheme: dark) use it, and
    // light-only pages get a heuristic inversion; images and video
    // are left alone.  This only ever recolors page CONTENT — Arora's
    // own widgets keep the desktop palette.
    engineSettings->setAttribute(QWebEngineSettings::ForceDarkMode,
        settings.value(QLatin1String("forceDarkMode"), false).toBool());

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

    // PING01: <a ping> auditing rides the same privacy/blockPings
    // toggle as the interceptor's sendBeacon/CSP-report drop — with
    // the attribute off Chromium never initiates the ping, and the
    // interceptor catches anything that still slips.
    engineSettings->setAttribute(QWebEngineSettings::HyperlinkAuditingEnabled,
        !QSettings().value(QLatin1String("privacy/blockPings"), true).toBool());

    // PRIV01: refresh the request interceptor's IO-thread snapshot
    // (https-first upgrade, referrer trim) alongside the profile
    // settings — the privacy group lives outside websettings/network.
    PrivacyRequestInterceptor::loadSettings();

    // DOH01: DNS-over-HTTPS is engine-global, not per-profile — it is
    // re-asserted here so a settings-dialog change reaches the
    // resolver for every profile without waiting for a restart (the
    // matching feature flag is process-lifetime, applied by
    // applyChromiumFlags in main()).
    applySecureDns();

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

    // REF01: page-level referrer policy for the legs the request
    // interceptor cannot write (redirect follow-ups).
    installReferrerPolicy(profile);

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
    profile->setHttpUserAgent(effectiveHttpUserAgent(userAgent));
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

// CONT05: the tree removal itself, shared by the sentinel wipe
// (clearDeferredSiteStorage) and by clearSiteStorageNow — neither may
// run while the owning profile is live.  includeNetworkState adds the
// exit-wipe dirs (Network/, Cache/, Code Cache/).
static bool removeSiteDataTrees(const QString &storagePath,
                                bool includeNetworkState)
{
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
    if (includeNetworkState) {
        for (const char *name : exitWipeDirs) {
            QDir dir(storagePath + QLatin1Char('/') + QLatin1String(name));
            if (dir.exists())
                ok &= dir.removeRecursively();
        }
    }
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

    bool ok = removeSiteDataTrees(storagePath, exitWipe);
    if (siteWipe)
        ok &= QFile::remove(sentinelPath);
    if (exitWipe)
        ok &= QFile::remove(exitSentinelPath);
    ensureUserOnlyPermissions(storagePath);
    return ok;
}

bool clearSiteStorageNow(const QString &storagePath)
{
    if (storagePath.isEmpty())
        return true;
    const bool ok = removeSiteDataTrees(storagePath, false);
    ensureUserOnlyPermissions(storagePath);
    return ok;
}

// DOH01: the stored DoH mode — 0 off, 1 automatic provider upgrade,
// 2 custom server with insecure fallback, 3 custom server strict.
// The PRIV01-era bool key folds into "automatic" so a settings file
// written before the mode selector keeps its meaning.
static int secureDnsModeSetting()
{
    const QVariant stored =
        QSettings().value(QLatin1String("privacy/secureDnsMode"));
    if (stored.isValid())
        return qBound(0, stored.toInt(), 3);
    return QSettings().value(QLatin1String("privacy/secureDns"), false)
        .toBool() ? 1 : 0;
}

// The DoH URI template for the custom modes.  An empty or invalid
// entry falls back to the default resolver rather than dropping to
// plain DNS: a mistyped field must not silently undo the "strict, no
// fallback" guarantee of mode 3.
static QString secureDnsServerSetting()
{
    QString server = QSettings().value(
        QLatin1String("privacy/secureDnsServer")).toString().trimmed();
    const QUrl url(server);
    if (!server.isEmpty() && url.scheme() == QLatin1String("https")
        && !url.host().isEmpty())
        return server;
    if (!server.isEmpty())
        qWarning() << "browserprofile: privacy/secureDnsServer" << server
                   << "is not an https:// URI — using the default resolver";
    return QLatin1String("https://cloudflare-dns.com/dns-query");
}

void applySecureDns()
{
    using QWebEngineGlobalSettings::SecureDnsMode;
    QWebEngineGlobalSettings::DnsMode dnsMode;
    switch (secureDnsModeSetting()) {
    case 2:
        dnsMode.secureMode = SecureDnsMode::SecureWithFallback;
        dnsMode.serverTemplates << secureDnsServerSetting();
        break;
    case 3:
        dnsMode.secureMode = SecureDnsMode::SecureOnly;
        dnsMode.serverTemplates << secureDnsServerSetting();
        break;
    default:
        // Modes 0/1 both mean SystemOnly at the resolver level — the
        // automatic upgrade (mode 1) is provider-list driven, armed by
        // the --enable-features=DnsOverHttps switch in
        // applyChromiumFlags(), and takes no template.  Calling with
        // SystemOnly also retracts a template applied earlier this
        // session when the user downgrades the mode.
        break;
    }
    if (!QWebEngineGlobalSettings::setDnsMode(dnsMode))
        qWarning() << "browserprofile: DoH configuration rejected:"
                   << dnsMode.serverTemplates;
}

void applyChromiumFlags()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("privacy"));
    const bool webrtcProtection =
        settings.value(QLatin1String("webrtcIpProtection"), true).toBool();
    const bool strictTlsCiphers =
        settings.value(QLatin1String("tlsStrictCiphers"), true).toBool();
    settings.endGroup();
    const bool secureDns = secureDnsModeSetting() != 0;

    // POL03: middle-click autoscroll is Chromium's own implementation
    // (the Blink MiddleClickAutoscroll feature) — it already
    // distinguishes link drags from empty-area scrolls, draws the
    // direction marker and respects a page's own middle-click
    // handlers.  On everywhere but macOS, where the convention does
    // not exist.  Process-lifetime switch — a settings change applies
    // at the next launch.
#if defined(Q_OS_MACOS)
    const bool autoscrollDefault = false;
#else
    const bool autoscrollDefault = true;
#endif
    const bool autoscroll = settings.value(
        QLatin1String("websettings/middleClickAutoscroll"),
        autoscrollDefault).toBool();

    QStringList flags = QString::fromLocal8Bit(
        qgetenv("QTWEBENGINE_CHROMIUM_FLAGS"))
        .split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const auto addFlag = [&flags](const QString &flag) {
        if (!flags.contains(flag))
            flags.append(flag);
    };
    // Comma-list switches (--enable-features, --enable-blink-features)
    // must MERGE rather than append: Chromium's command line keeps
    // only the last occurrence of a switch, so a second
    // --enable-features=… would silently drop the operator's list.
    const auto addListEntry = [&flags](const QString &name,
                                       const QString &entry) {
        const QString prefix = name + QLatin1Char('=');
        for (QString &flag : flags) {
            if (!flag.startsWith(prefix))
                continue;
            const QStringList entries = flag.mid(prefix.size()).split(
                QLatin1Char(','), Qt::SkipEmptyParts);
            if (!entries.contains(entry))
                flag += QLatin1Char(',') + entry;
            return;
        }
        flags.append(prefix + entry);
    };

    // TELEM01: silence the engine's unsolicited background traffic.
    // Chromium otherwise fetches field-trial/variations seeds, runs a
    // component updater, uploads domain-reliability reports and usage
    // metrics, and pings at first run — none of which the user asked
    // for.  These switches only cut silent channels; no user-visible
    // feature depends on them.
    addFlag(QLatin1String("--disable-background-networking"));
    addFlag(QLatin1String("--disable-component-update"));
    addFlag(QLatin1String("--disable-domain-reliability"));
    addFlag(QLatin1String("--disable-metrics"));
    addFlag(QLatin1String("--disable-sync"));
    addFlag(QLatin1String("--no-first-run"));

    // POL02: the offscreen QPA has no display/GPU to composite on and
    // Chromium then produces no frames at all — every view grab (tab
    // hover previews, thumbnails) comes back blank.  Software
    // rasterizing keeps headless runs fully rendered.
    if (qEnvironmentVariable("QT_QPA_PLATFORM")
            == QLatin1String("offscreen"))
        addFlag(QLatin1String("--disable-gpu"));

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
        // The DoH feature gate.  Automatic mode (1) is armed by this
        // switch alone — Chromium upgrades when the network resolver
        // is on its known-provider list.  The custom modes (2/3)
        // additionally push their endpoint through
        // QWebEngineGlobalSettings::setDnsMode in applySecureDns().
        addListEntry(QLatin1String("--enable-features"),
                     QLatin1String("DnsOverHttps"));
    }
    if (autoscroll) {
        // POL03: see the default comment above — this arms Blink's
        // built-in grab-scroll (middle-press on empty content then
        // move the pointer; releasing or another click cancels it).
        addListEntry(QLatin1String("--enable-blink-features"),
                     QLatin1String("MiddleClickAutoscroll"));
    }
    if (strictTlsCiphers) {
        // TLS01: strip the weak suites from the ClientHello — RSA key
        // exchange has no forward secrecy and CBC is weak — leaving
        // TLS 1.3 plus the ECDHE+AEAD set advertised.  This only
        // narrows what we OFFER; negotiated strong suites are
        // unaffected.  Feature-checked on the bundled Chromium: the
        // switch still reaches the network service's SSLConfig and
        // drops exactly these suites (probe: local TLS capture of the
        // advertised list).  An operator-supplied
        // --cipher-suite-blacklist in the environment wins over the
        // built-in list.
        const QLatin1String prefix("--cipher-suite-blacklist=");
        bool alreadySet = false;
        for (const QString &flag : flags)
            alreadySet |= flag.startsWith(prefix);
        if (!alreadySet)
            flags.append(prefix
                + QLatin1String("0x009c,0x009d,0x002f,0x0035,0xc013,0xc014"));
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
