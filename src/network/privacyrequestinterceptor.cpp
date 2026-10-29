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

#include "privacyrequestinterceptor.h"
#include "profilesettings.h"

#include "adblockrequestinterceptor.h"
#include "browserpaths.h"
#include "browserprofile.h"
#include "declarativenetrequest.h"
#include "extensionrequestwatcher.h"
#include "navigationpolicy.h"
#include "policyrequestmarshal.h"
#include "scriptcontrolmanager.h"

#include <qatomic.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qmutex.h>
#include <qsettings.h>
#include <qwebengineprofile.h>
#include <qwebengineurlrequestinfo.h>

#if defined(ARKTOS_RUSTCORE)
#include <rustcore.h>
#include <arktos_core.h>
#include "rustcorebridge.h"
#include "sitedecisionstore.h"
#endif

// #define PRIVACYINTERCEPTOR_DEBUG
#if defined(PRIVACYINTERCEPTOR_DEBUG)
#include <qdebug.h>
#endif

// ENG02: this class is now a thin ADAPTER — it marshals each request
// into an engine-neutral PolicyRequest, delegates EVERY decision to
// Engine::NavigationPolicy (rustcore when linked, the Qt fallback
// otherwise), and applies the verdict.  The adblock matcher remains
// the delegated tail stage on an "allow" verdict.  No branching
// policy logic lives here anymore — settings storage (QSettings)
// and the JSCTL lookup stay Qt-side as inputs, never as decisions.

using Engine::HeaderList;
using Engine::NavigationPolicy;
using Engine::PolicyRequest;
using Engine::PolicyVerdict;

// PDF01 audit probe — counts every request the interceptor sees.
static QAtomicInteger<qint64> s_requestsSeen;

// POL04 per-page cleaning tally — written from the IO thread by the
// clearnet and tor interceptors, read on the GUI thread by the
// shield panel.  A cleaning redirect drops N tracker units and
// records its target as the host's "marked" URL: the follow-up
// request for that URL is the same navigation, so it keeps the
// count; any other main-frame request to the host settles the entry
// (a fresh page load starts from zero).  Chained redirects carry the
// marker forward — a second redirect leg simply re-marks its target.
// Bounded like the adblock table.
struct CleanTally {
    int removed = 0;
    QByteArray markedUrl;   // last redirect target — the load's URL
};
static QMutex s_cleanedCountLock;
static QHash<QString, CleanTally> s_cleanedCounts;
static const int maxCleanedCountHosts = 512;

void PrivacyRequestInterceptor::noteMainFrameRequest(const QUrl &url)
{
    const QString host = url.host().toLower();
    if (host.isEmpty())
        return;
    const QMutexLocker lock(&s_cleanedCountLock);
    auto it = s_cleanedCounts.find(host);
    if (it == s_cleanedCounts.end())
        return;
    if (!it->markedUrl.isEmpty()
        && it->markedUrl == url.toEncoded()) {
        // The follow-up request for the URL a cleaning redirect just
        // produced — same page load; consume the marker, keep count.
        it->markedUrl.clear();
    } else {
        // A navigation that cleaning did not produce — the previous
        // page's count no longer describes this host's current page.
        s_cleanedCounts.erase(it);
    }
}

void PrivacyRequestInterceptor::noteCleanedRedirect(const QUrl &to,
                                                    int removed)
{
    const QString host = to.host().toLower();
    if (host.isEmpty())
        return;
    const QMutexLocker lock(&s_cleanedCountLock);
    auto it = s_cleanedCounts.find(host);
    if (it == s_cleanedCounts.end()) {
        if (removed <= 0)
            return;   // a plain redirect carries no tally or marker
        if (s_cleanedCounts.size() >= maxCleanedCountHosts)
            return;
        it = s_cleanedCounts.insert(host, CleanTally());
    }
    it->removed += removed;
    it->markedUrl = to.toEncoded();
}

int PrivacyRequestInterceptor::trackersRemovedCount(const QString &host)
{
    const QMutexLocker lock(&s_cleanedCountLock);
    return s_cleanedCounts.value(host.toLower()).removed;
}

void PrivacyRequestInterceptor::resetTrackerCounts()
{
    const QMutexLocker lock(&s_cleanedCountLock);
    s_cleanedCounts.clear();
}

static PolicyRequest buildRequest(QWebEngineUrlRequestInfo &info,
                                  const QString &scope, bool torMode,
                                  int minRefererLevel)
{
    PolicyRequest req = PolicyRequestMarshal::fromRequestInfo(info);
    req.scope = scope;
    req.torMode = torMode;
    req.minRefererLevel = minRefererLevel;
    return req;
}

// Applies the verdict; returns false when the request was terminated
// (blocked or redirected), true when the delegated matcher tail
// should run.
static bool applyVerdict(const PolicyVerdict &verdict,
                         QWebEngineUrlRequestInfo &info)
{
    switch (verdict.action) {
    case PolicyVerdict::Action::Pass:
        return false;
    case PolicyVerdict::Action::Block:
#if defined(PRIVACYINTERCEPTOR_DEBUG)
        qDebug() << "PrivacyRequestInterceptor: block"
                 << info.requestUrl() << "reason" << verdict.reason;
#endif
        info.block(true);
        return false;
    case PolicyVerdict::Action::Redirect:
#if defined(PRIVACYINTERCEPTOR_DEBUG)
        qDebug() << "PrivacyRequestInterceptor: redirect"
                 << info.requestUrl() << "->" << verdict.redirectUrl
                 << verdict.reason;
#endif
        info.redirect(verdict.redirectUrl);
        // POL04: tally the cleaning stage's dropped tracker units
        // under the destination host — the page the user actually
        // lands on (for an unwrap that is the real target, not the
        // redirector) — and mark the target so its follow-up request
        // does not settle the fresh count.  Main-frame only: a
        // subresource strip is not the page's navigation cleaning,
        // and its memoized verdict would re-tally on every hit.
        if (info.resourceType()
            == QWebEngineUrlRequestInfo::ResourceTypeMainFrame)
            PrivacyRequestInterceptor::noteCleanedRedirect(
                verdict.redirectUrl, verdict.cleanedCount);
        return false;
    case PolicyVerdict::Action::Allow:
        if (verdict.refererSet)
            info.setHttpHeader("Referer", verdict.refererValue);
        // PRIV08: Global Privacy Control — the verdict's sendGpc bit
        // already folded the privacy/gpcSignal toggle (and the tor
        // pipeline's unconditional override) in.  setHttpHeader is
        // the REF01-verified path that reaches the wire on main-frame
        // and subresource legs alike; a false bit writes NOTHING —
        // absence is the spec's no-preference state.
        if (verdict.sendGpc)
            info.setHttpHeader("Sec-GPC", "1");
        return true;
    }
    return true;
}

PrivacyRequestInterceptor::PrivacyRequestInterceptor(AdBlockNetwork *network,
                                                     QWebEngineProfile *profile)
    : QWebEngineUrlRequestInterceptor(profile)
    , m_adBlock(new AdBlockRequestInterceptor(network, this))
    , m_scope(downgradeScope(profile))
    , m_profileId(BrowserProfile::profileIdFor(profile))
    // ER11: extension request observation is a normal-profile feed —
    // off-the-record browsing hosts no extensions, so its traffic
    // must never reach an extension context.
    , m_reportToExtensions(profile && !profile->isOffTheRecord())
{
}

void PrivacyRequestInterceptor::interceptRequest(QWebEngineUrlRequestInfo &info)
{
    // PDF01 audit probe — counts every request the interceptor sees.
    s_requestsSeen.fetchAndAddRelaxed(1);

    // POL04: a fresh top-level request settles the host's cleaning
    // tally — unless it is the marked target a cleaning redirect just
    // produced (same load; the count carries into the page).
    if (info.resourceType() == QWebEngineUrlRequestInfo::ResourceTypeMainFrame)
        noteMainFrameRequest(info.requestUrl());

    // Runs on the WebEngine IO thread — only the lock-guarded policy
    // state may be read here (inside NavigationPolicy / rustcore).
    const PolicyRequest req =
        buildRequest(info, m_scope, /*torMode=*/false,
                     RefererEngineDefault);
    PolicyVerdict verdict = NavigationPolicy::evaluate(req);
    // MD05: the verdict's Sec-GPC bit came off the process-global
    // policy snapshot — re-resolve it against THIS profile's own
    // privacy store (rc_privacy_gpc, generation-cached).  When no
    // facade answers the bit stands untouched.
    verdict.sendGpc = sendGpcForProfile(verdict.sendGpc);
    // ER11: announce the request to observe-only webRequest listeners
    // BEFORE the verdict applies — Chrome's onBeforeRequest fires for
    // requests other features later drop — and pair a policy block
    // with onErrorOccurred.
    if (m_reportToExtensions)
        ExtensionRequestWatcher::observeRequest(info);
    // MD34: chrome.declarativeNetRequest — evaluated here on the IO
    // thread over the lock-guarded rule snapshot, the same stage
    // Chrome runs DNR before onBeforeSendHeaders.  A block or
    // redirect short-circuits the policy/adblock tail; request-header
    // ops apply only while the request survives.
    bool dnrHandled = false;
    if (m_reportToExtensions
            && DeclarativeNetRequest::registeredRuleCount() > 0) {
        DeclarativeNetRequest::EvalInput dnrIn;
        dnrIn.url = info.requestUrl();
        dnrIn.firstPartyUrl = info.firstPartyUrl();
        dnrIn.resourceType = int(info.resourceType());
        dnrIn.method = info.requestMethod();
        const DeclarativeNetRequest::Verdict dnr =
            DeclarativeNetRequest::evaluate(dnrIn);
        if (dnr.blocked) {
            info.block(true);
            ExtensionRequestWatcher::observeBlocked(info);
            dnrHandled = true;
        } else if (!dnr.redirectUrl.isEmpty()) {
            info.redirect(dnr.redirectUrl);
            dnrHandled = true;
        } else {
            for (const DeclarativeNetRequest::RequestHeaderOp &op
                 : dnr.headerOps) {
                if (op.op == DeclarativeNetRequest::HeaderAppend) {
                    const QByteArray current =
                        info.httpHeaders().value(op.name);
                    info.setHttpHeader(
                        op.name,
                        current.isEmpty()
                            ? op.value : current + ", " + op.value);
                } else {
                    // 'set' overwrites; 'remove' clears the header —
                    // the engine has no true header-delete verb.
                    info.setHttpHeader(op.name, op.value);
                }
            }
        }
    }
    if (!dnrHandled && applyVerdict(verdict, info)) {
        // "allow" — per-site UA spoof, then the adblock matcher as
        // the delegated tail stage.
        applyUserAgentSpoof(info);
        m_adBlock->interceptRequest(info);
    } else if (!dnrHandled && m_reportToExtensions
               && verdict.action == PolicyVerdict::Action::Block) {
        ExtensionRequestWatcher::observeBlocked(info);
    }
}

void PrivacyRequestInterceptor::loadSettings()
{
    ProfileSettings settings;
    settings.beginGroup(QLatin1String("privacy"));

    NavigationPolicy::Snapshot snap;
    bool haveFlags = false;
#if defined(ARKTOS_RUSTCORE)
    // MD05: the privacy/* toggles are persona rows in the bound
    // profile's settings store — resolve them through the privacy
    // module so every adapter reads the same bytes; the direct
    // ProfileSettings consult below stays the no-bridge fallback.
    const QJsonObject pf = privacyStoreCall(
        BrowserPaths::activeProfileId(),
        {{QLatin1String("op"), QLatin1String("flags")}});
    if (!pf.isEmpty()) {
        haveFlags = true;
        snap.httpsFirst =
            pf.value(QLatin1String("https_first")).toBool(true);
        snap.httpsOnly =
            pf.value(QLatin1String("https_only")).toBool(true);
        snap.refererPolicy = qBound(
            0, pf.value(QLatin1String("referer_policy")).toInt(1), 3);
        snap.securityLevel = qBound(
            int(PrivacyRequestInterceptor::Standard),
            pf.value(QLatin1String("security_level")).toInt(0),
            int(PrivacyRequestInterceptor::Safest));
        snap.blockPings =
            pf.value(QLatin1String("block_pings")).toBool(true);
        snap.blockRemoteFonts =
            pf.value(QLatin1String("block_remote_fonts")).toBool(false);
        snap.blockPrefetch =
            pf.value(QLatin1String("block_prefetch")).toBool(true);
        snap.blockThirdPartyWebSockets =
            pf.value(QLatin1String("block_third_party_ws")).toBool(false);
        snap.stripTrackingParams =
            pf.value(QLatin1String("strip_tracking_params")).toBool(true);
        snap.domainBlocklist =
            pf.value(QLatin1String("domain_blocklist")).toBool(true);
        snap.gpcSignal =
            pf.value(QLatin1String("gpc_signal")).toBool(true);
    }
    // MD06: the persona's merged urlclean ruleset (profile override
    // or the shared global set, extraParams folded in) rides the
    // snapshot — rc_policy_load_snapshot parses it once, so the
    // request path never re-reads the profile store.  An empty
    // answer keeps the process-global urlstrip set.
    const QJsonObject uc = urlcleanStoreCall(
        BrowserPaths::activeProfileId(),
        {{QLatin1String("op"), QLatin1String("clean_policy")}});
    if (uc.contains(QLatin1String("ruleset")))
        snap.urlCleanRuleset =
            uc.value(QLatin1String("ruleset")).toObject();
#endif
    if (!haveFlags) {
        snap.httpsFirst = settings.value(QLatin1String("httpsFirst"), true).toBool();
        snap.httpsOnly = settings.value(QLatin1String("httpsOnly"), true).toBool();
        snap.refererPolicy = storedRefererPolicy();
        snap.securityLevel = qBound(
            int(PrivacyRequestInterceptor::Standard),
            settings.value(QLatin1String("securityLevel"),
                           int(PrivacyRequestInterceptor::Standard)).toInt(),
            int(PrivacyRequestInterceptor::Safest));
        snap.blockPings = settings.value(QLatin1String("blockPings"), true).toBool();
        snap.blockRemoteFonts =
            settings.value(QLatin1String("blockRemoteFonts"), false).toBool();
        snap.blockPrefetch =
            settings.value(QLatin1String("blockPrefetch"), true).toBool();
        snap.blockThirdPartyWebSockets =
            settings.value(QLatin1String("blockThirdPartyWebSockets"), false).toBool();
        // POL04: privacy/urlCleaning is the canonical toggle (Settings >
        // Privacy "Clean tracking links"); the SEC17-era
        // stripTrackingParams key is the legacy alias an older profile
        // may still carry.
        snap.stripTrackingParams = settings.value(
            QLatin1String("urlCleaning"),
            settings.value(QLatin1String("stripTrackingParams"), true))
            .toBool();
        snap.domainBlocklist =
            settings.value(QLatin1String("domainBlocklist"), true).toBool();
        snap.gpcSignal =
            settings.value(QLatin1String("gpcSignal"), true).toBool();
    }
#if defined(ARKTOS_RUSTCORE)
    // SITED01: one-shot import — replay the legacy exception list into
    // the Rust store and retire the key after the writes land.
    const QStringList legacyExceptions =
        settings.value(QLatin1String("httpsOnlyExceptions")).toStringList();
    bool allWritten = true;
    for (const QString &host : legacyExceptions) {
        const QString lowered = host.toLower();
        if (!lowered.isEmpty())
            allWritten = SiteDecisionStore::set(
                SiteDecisionStore::KindHttpAllow,
                lowered, QLatin1String("allow")) && allWritten;
    }
    if (!legacyExceptions.isEmpty() && allWritten)
        settings.remove(QLatin1String("httpsOnlyExceptions"));
    const QHash<QString, QString> rows =
        SiteDecisionStore::entries(SiteDecisionStore::KindHttpAllow);
    for (auto it = rows.constBegin(); it != rows.constEnd(); ++it) {
        const QString lowered = it.key().toLower();
        if (!lowered.isEmpty())
            snap.httpsOnlyExceptions.append(lowered);
    }
    // POL04: per-site link-cleaning exemptions persist as "urlclean"
    // rows in the same store.
    const QHash<QString, QString> cleanRows =
        SiteDecisionStore::entries(SiteDecisionStore::KindUrlClean);
    for (auto it = cleanRows.constBegin(); it != cleanRows.constEnd(); ++it) {
        const QString lowered = it.key().toLower();
        if (!lowered.isEmpty())
            snap.urlCleanExceptions.append(lowered);
    }
#else
    snap.httpsOnlyExceptions =
        settings.value(QLatin1String("httpsOnlyExceptions")).toStringList();
    snap.urlCleanExceptions =
        settings.value(QLatin1String("urlCleanExceptions")).toStringList();
#endif
    settings.endGroup();

    NavigationPolicy::loadSnapshot(snap);
}

QString PrivacyRequestInterceptor::downgradeScope(
        const QWebEngineProfile *profile)
{
    if (!profile)
        return QString();
    if (!profile->isOffTheRecord())
        return profile->storageName();
    // Unnamed/off-the-record profiles carry no storageName — the
    // pointer is the only identity; marks die with the process anyway.
    return QLatin1String("otr:")
        + QString::number(reinterpret_cast<quintptr>(profile));
}

// MD05: one granular call into rustcore's privacy module — the
// per-persona privacy-policy store view (the privacy/* toggles,
// the Sec-GPC bit, the tracker-strip flag and the unified adblock
// read).  The profile-bound AcPrivacy is minted per call like
// AcDlwpolicy — cheap, and a fresh settings open per op reads the
// file as committed, so a settings-dialog save mid-process is
// honored.  An empty object means no facade answered — callers
// then run their local QSettings consult, so an FFI failure can
// never silently flip a protection.
QJsonObject PrivacyRequestInterceptor::privacyStoreCall(
        const QString &profileId, const QJsonObject &request)
{
#if defined(ARKTOS_RUSTCORE)
    AcPrivacy *h = nullptr;
    if (RustCoreBridge *bridge = RustCoreBridge::serving())
        h = bridge->privacy(profileId);
    if (!h)
        return QJsonObject();
    const QByteArray json =
        QJsonDocument(request).toJson(QJsonDocument::Compact);
    char *raw = ac_privacy_call(
        h, reinterpret_cast<const uint8_t *>(json.constData()),
        size_t(json.size()));
    ac_privacy_close(h);
    if (!raw)
        return QJsonObject();
    QByteArray out = QByteArray::fromRawData(raw, int(strlen(raw)));
    out.detach();
    rc_string_free(raw);
    return QJsonDocument::fromJson(out).object();
#else
    Q_UNUSED(profileId);
    Q_UNUSED(request);
    return QJsonObject();
#endif
}

// MD06: one granular call into rustcore's urlclean module — the
// per-persona URL-sanitize policy (the editable ruleset override,
// the copy-path toggle, the clean_policy composite).  Same shape as
// privacyStoreCall: a profile-bound AcUrlclean is minted per call,
// and an empty object means no facade answered — the caller keeps
// its local consult, so an FFI failure can never silently flip a
// cleaning decision.
QJsonObject PrivacyRequestInterceptor::urlcleanStoreCall(
        const QString &profileId, const QJsonObject &request)
{
#if defined(ARKTOS_RUSTCORE)
    AcUrlclean *h = nullptr;
    if (RustCoreBridge *bridge = RustCoreBridge::serving())
        h = bridge->urlclean(profileId);
    if (!h)
        return QJsonObject();
    const QByteArray json =
        QJsonDocument(request).toJson(QJsonDocument::Compact);
    char *raw = ac_urlclean_call(
        h, reinterpret_cast<const uint8_t *>(json.constData()),
        size_t(json.size()));
    ac_urlclean_close(h);
    if (!raw)
        return QJsonObject();
    QByteArray out = QByteArray::fromRawData(raw, int(strlen(raw)));
    out.detach();
    rc_string_free(raw);
    return QJsonDocument::fromJson(out).object();
#else
    Q_UNUSED(profileId);
    Q_UNUSED(request);
    return QJsonObject();
#endif
}

bool PrivacyRequestInterceptor::sendGpcForProfile(bool verdictBit)
{
#if defined(ARKTOS_RUSTCORE)
    // Cache the per-persona answer against the verdict-state
    // generation — a privacy-toggle save pushes a fresh policy
    // snapshot (bumping the generation), so the bound profile's
    // settings.json is re-read at most once per generation here
    // instead of once per request.
    const quint64 generation = ac_policy_eval_generation();
    {
        const QMutexLocker lock(&m_gpcLock);
        if (m_gpcGen == generation)
            return m_gpcSend;
    }
    int answer = -1;
    if (RustCoreBridge *bridge = RustCoreBridge::serving()) {
        AcPrivacy *h = bridge->privacy(m_profileId);
        if (h) {
            answer = ac_privacy_gpc(h, /*tor_mode=*/0);
            ac_privacy_close(h);
        }
    }
    if (answer < 0)
        return verdictBit;
    const QMutexLocker lock(&m_gpcLock);
    m_gpcSend = (answer == 1);
    m_gpcGen = generation;
    return m_gpcSend;
#else
    return verdictBit;
#endif
}

int PrivacyRequestInterceptor::storedRefererPolicy()
{
#if defined(ARKTOS_RUSTCORE)
    const QJsonObject r = privacyStoreCall(
        BrowserPaths::activeProfileId(),
        {{QLatin1String("op"), QLatin1String("flag")},
         {QLatin1String("name"), QLatin1String("referer_policy")}});
    if (r.contains(QLatin1String("value")))
        return qBound(0, r.value(QLatin1String("value")).toInt(), 3);
#endif
    ProfileSettings settings;
    settings.beginGroup(QLatin1String("privacy"));
    // REF01: the PRIV01 bool folds into the level selector — an old
    // "trimReferer = false" maps to EngineDefault, anything else to
    // the Trimmed default.
    const QVariant storedPolicy =
        settings.value(QLatin1String("refererPolicy"));
    const int level = storedPolicy.isValid()
        ? storedPolicy.toInt()
        : (settings.value(QLatin1String("trimReferer"), true).toBool()
               ? int(RefererTrimmed) : int(RefererEngineDefault));
    settings.endGroup();
    return qBound(int(RefererEngineDefault), level, int(RefererNever));
}

// ---- delegated decision surface (1:1 with the policy) -----------------

bool PrivacyRequestInterceptor::httpsFirstEnabled()
{
    return NavigationPolicy::httpsFirstEnabled();
}

bool PrivacyRequestInterceptor::trimRefererEnabled()
{
    return NavigationPolicy::refererPolicy() != RefererEngineDefault;
}

int PrivacyRequestInterceptor::refererPolicy()
{
    return NavigationPolicy::refererPolicy();
}

QByteArray PrivacyRequestInterceptor::referrerMetaValue(int level)
{
    return NavigationPolicy::referrerMetaValue(level);
}

int PrivacyRequestInterceptor::securityLevel()
{
    return NavigationPolicy::securityLevel();
}

bool PrivacyRequestInterceptor::blockPingsEnabled()
{
    return NavigationPolicy::blockPingsEnabled();
}

bool PrivacyRequestInterceptor::blockRemoteFontsEnabled()
{
    return NavigationPolicy::blockRemoteFontsEnabled();
}

bool PrivacyRequestInterceptor::blockPrefetchEnabled()
{
    return NavigationPolicy::blockPrefetchEnabled();
}

bool PrivacyRequestInterceptor::blockThirdPartyWebSocketsEnabled()
{
    return NavigationPolicy::blockThirdPartyWebSocketsEnabled();
}

bool PrivacyRequestInterceptor::stripTrackingParamsEnabled()
{
    return NavigationPolicy::stripTrackingParamsEnabled();
}

bool PrivacyRequestInterceptor::isDomainBlocked(const QString &host)
{
    return NavigationPolicy::isDomainBlocked(host);
}

bool PrivacyRequestInterceptor::domainBlocklistEnabled()
{
    return NavigationPolicy::domainBlocklistEnabled();
}

bool PrivacyRequestInterceptor::gpcSignalEnabled()
{
    return NavigationPolicy::gpcSignalEnabled();
}

bool PrivacyRequestInterceptor::storedGpcSignal()
{
    return gpcSignalEnabledForProfile(QString());
}

bool PrivacyRequestInterceptor::gpcSignalEnabledForProfile(
        const QString &profileId)
{
    const QString pid = BrowserPaths::isProfileId(profileId)
        ? profileId : BrowserPaths::activeProfileId();
#if defined(ARKTOS_RUSTCORE)
    // MD05: the persona's own privacy store answers — the ambient
    // profile's row would mislead a second persona's engine profile
    // (installGpcSignal serves whichever profile it is handed).
    const QJsonObject r = privacyStoreCall(
        pid, {{QLatin1String("op"), QLatin1String("gpc")}});
    if (r.contains(QLatin1String("send")))
        return r.value(QLatin1String("send")).toBool();
#endif
    ProfileSettings settings(pid);
    settings.beginGroup(QLatin1String("privacy"));
    const bool on = settings.value(QLatin1String("gpcSignal"), true).toBool();
    settings.endGroup();
    return on;
}

bool PrivacyRequestInterceptor::shouldBlockDomain(const QUrl &url)
{
    return NavigationPolicy::shouldBlockDomain(url);
}

bool PrivacyRequestInterceptor::isBlockedDomainAllowed(const QString &host)
{
    return NavigationPolicy::isBlockedDomainAllowed(host);
}

void PrivacyRequestInterceptor::allowBlockedDomain(const QString &host)
{
    NavigationPolicy::allowBlockedDomain(host);
}

void PrivacyRequestInterceptor::clearBlockedDomainAllowance(
        const QString &host)
{
    NavigationPolicy::clearBlockedDomainAllowance(host);
}

void PrivacyRequestInterceptor::clearBlockedDomainAllowances()
{
    NavigationPolicy::clearBlockedDomainAllowances();
}

void PrivacyRequestInterceptor::recordBlockedDomainNav(const QUrl &url)
{
    NavigationPolicy::recordBlockedDomainNav(url);
}

bool PrivacyRequestInterceptor::takeBlockedDomainNav(const QUrl &url)
{
    return NavigationPolicy::takeBlockedDomainNav(url);
}

QUrl PrivacyRequestInterceptor::strippedUrl(const QUrl &url)
{
    return NavigationPolicy::strippedUrl(url);
}

bool PrivacyRequestInterceptor::shouldBlockResource(
        QWebEngineUrlRequestInfo::ResourceType type,
        const QHash<QByteArray, QByteArray> &headers)
{
    return NavigationPolicy::shouldBlockResource(
        static_cast<int>(type),
        PolicyRequestMarshal::toHeaderList(headers));
}

bool PrivacyRequestInterceptor::isPrivateOrLocalHost(const QString &host)
{
    return NavigationPolicy::isPrivateOrLocalHost(host);
}

bool PrivacyRequestInterceptor::failureImpliesDowngrade(int errorDomain,
                                                        int errorCode)
{
    return NavigationPolicy::failureImpliesDowngrade(errorDomain, errorCode);
}

bool PrivacyRequestInterceptor::isUpgradeCandidate(const QUrl &url,
                                                   const QString &scope)
{
    return NavigationPolicy::isUpgradeCandidate(url, scope);
}

bool PrivacyRequestInterceptor::isDowngraded(const QString &host,
                                             const QString &scope)
{
    return NavigationPolicy::isDowngraded(host, scope);
}

bool PrivacyRequestInterceptor::noteNavigationFailure(const QUrl &url,
        int errorDomain, int errorCode, const QString &scope)
{
    return NavigationPolicy::noteNavigationFailure(url, errorDomain,
                                                   errorCode, scope);
}

void PrivacyRequestInterceptor::markDowngraded(const QString &host,
                                               const QString &scope)
{
    NavigationPolicy::markDowngraded(host, scope);
}

void PrivacyRequestInterceptor::clearDowngradedHost(const QString &host,
                                                    const QString &scope)
{
    NavigationPolicy::clearDowngradedHost(host, scope);
}

void PrivacyRequestInterceptor::clearDowngradedHosts()
{
    NavigationPolicy::clearDowngradedHosts();
}

qint64 PrivacyRequestInterceptor::downgradeTtlMs()
{
    return NavigationPolicy::downgradeTtlMs();
}

void PrivacyRequestInterceptor::setDowngradeTtlMs(qint64 ms)
{
    NavigationPolicy::setDowngradeTtlMs(ms);
}

bool PrivacyRequestInterceptor::httpsOnlyEnabled()
{
    return NavigationPolicy::httpsOnlyEnabled();
}

bool PrivacyRequestInterceptor::shouldWarnHttp(const QUrl &url,
                                               const QString &scope)
{
    return NavigationPolicy::shouldWarnHttp(url, scope);
}

bool PrivacyRequestInterceptor::isHttpAllowedHost(const QString &host)
{
    return NavigationPolicy::isHttpAllowedHost(host);
}

void PrivacyRequestInterceptor::allowHttpForHost(const QString &host,
                                                 bool persistent)
{
    const QString lowered = host.toLower();
    if (lowered.isEmpty())
        return;
    // The policy keeps the host sets; the decision store (or
    // QSettings in no-rust builds) persists them.
    const bool persistNeeded =
        NavigationPolicy::allowHttpForHost(lowered, persistent);
    if (!persistent || !persistNeeded)
        return;
#if defined(ARKTOS_RUSTCORE)
    SiteDecisionStore::set(SiteDecisionStore::KindHttpAllow,
                           lowered, QLatin1String("allow"));
#else
    ProfileSettings settings;
    settings.beginGroup(QLatin1String("privacy"));
    QStringList exceptions =
        settings.value(QLatin1String("httpsOnlyExceptions")).toStringList();
    if (!exceptions.contains(lowered)) {
        exceptions.append(lowered);
        settings.setValue(QLatin1String("httpsOnlyExceptions"), exceptions);
    }
    settings.endGroup();
#endif
}

void PrivacyRequestInterceptor::clearHttpAllowance(const QString &host)
{
    const QString lowered = host.toLower();
    NavigationPolicy::clearHttpAllowance(lowered);
#if defined(ARKTOS_RUSTCORE)
    SiteDecisionStore::remove(SiteDecisionStore::KindHttpAllow, lowered);
#else
    ProfileSettings settings;
    settings.beginGroup(QLatin1String("privacy"));
    QStringList exceptions =
        settings.value(QLatin1String("httpsOnlyExceptions")).toStringList();
    if (exceptions.removeAll(lowered) > 0)
        settings.setValue(QLatin1String("httpsOnlyExceptions"), exceptions);
    settings.endGroup();
#endif
}

QStringList PrivacyRequestInterceptor::httpExceptionHosts()
{
    return NavigationPolicy::httpExceptionHosts();
}

// POL04: the shield panel's "Clean tracking links" row — the policy
// keeps the live host sets, the store (or QSettings in no-rust
// builds) persists the durable half, exactly like
// allowHttpForHost/clearHttpAllowance above.
bool PrivacyRequestInterceptor::isUrlCleanExempt(const QString &host)
{
    return NavigationPolicy::isUrlCleanExempt(host);
}

void PrivacyRequestInterceptor::allowUrlCleanExempt(const QString &host,
                                                    bool persistent)
{
    const QString lowered = host.toLower();
    if (lowered.isEmpty())
        return;
    const bool persistNeeded =
        NavigationPolicy::allowUrlCleanExempt(lowered, persistent);
    if (!persistent || !persistNeeded)
        return;
#if defined(ARKTOS_RUSTCORE)
    SiteDecisionStore::set(SiteDecisionStore::KindUrlClean,
                           lowered, QLatin1String("exempt"));
#else
    QSettings settings;
    settings.beginGroup(QLatin1String("privacy"));
    QStringList exceptions =
        settings.value(QLatin1String("urlCleanExceptions")).toStringList();
    if (!exceptions.contains(lowered)) {
        exceptions.append(lowered);
        settings.setValue(QLatin1String("urlCleanExceptions"), exceptions);
    }
    settings.endGroup();
#endif
}

void PrivacyRequestInterceptor::clearUrlCleanExempt(const QString &host)
{
    const QString lowered = host.toLower();
    NavigationPolicy::clearUrlCleanExempt(lowered);
#if defined(ARKTOS_RUSTCORE)
    SiteDecisionStore::remove(SiteDecisionStore::KindUrlClean, lowered);
#else
    QSettings settings;
    settings.beginGroup(QLatin1String("privacy"));
    QStringList exceptions =
        settings.value(QLatin1String("urlCleanExceptions")).toStringList();
    if (exceptions.removeAll(lowered) > 0)
        settings.setValue(QLatin1String("urlCleanExceptions"), exceptions);
    settings.endGroup();
#endif
}

QStringList PrivacyRequestInterceptor::urlCleanExemptHosts()
{
    return NavigationPolicy::urlCleanExemptHosts();
}

void PrivacyRequestInterceptor::recordBlockedHttpNav(const QUrl &url)
{
    NavigationPolicy::recordBlockedHttpNav(url);
}

bool PrivacyRequestInterceptor::takeBlockedHttpNav(const QUrl &url)
{
    return NavigationPolicy::takeBlockedHttpNav(url);
}

bool PrivacyRequestInterceptor::shouldWarnFormPost(const QUrl &url,
                                                   const QString &scope)
{
    return NavigationPolicy::shouldWarnFormPost(url, scope);
}

bool PrivacyRequestInterceptor::shouldBlockScript(
        const QUrl &firstPartyUrl,
        QWebEngineUrlRequestInfo::ResourceType type)
{
    const bool scriptAllowed = ScriptControlManager::isAllowedHostSnapshot(
        firstPartyUrl.host());
    return NavigationPolicy::shouldBlockScript(
        firstPartyUrl, static_cast<int>(type), scriptAllowed);
}

bool PrivacyRequestInterceptor::shouldBlockWebSocket(
        const QUrl &firstPartyUrl, const QUrl &requestUrl,
        QWebEngineUrlRequestInfo::ResourceType type)
{
    return NavigationPolicy::shouldBlockWebSocket(
        firstPartyUrl, requestUrl, static_cast<int>(type));
}

void PrivacyRequestInterceptor::applyRefererPolicy(
        QWebEngineUrlRequestInfo &info, int minimumLevel)
{
    const QByteArray refHeader = info.httpHeaders().value("Referer");
    QByteArray value;
    if (NavigationPolicy::refererForRequest(
            QString::fromUtf8(info.requestUrl().toEncoded()),
            refHeader, minimumLevel, value)) {
        info.setHttpHeader("Referer", value);
    }
}

QByteArray PrivacyRequestInterceptor::rewrittenReferer(
        int level, const QUrl &source, const QUrl &target)
{
    return NavigationPolicy::rewrittenReferer(level, source, target);
}

qint64 PrivacyRequestInterceptor::requestsSeen()
{
    return s_requestsSeen.loadRelaxed();
}

void PrivacyRequestInterceptor::resetRequestsSeen()
{
    s_requestsSeen.storeRelaxed(0);
}

// UAG01: per-site UA spoof — rustcore's "uaspoof" site-decision table.
// A hit is sent as the request's User-Agent header; subresources to the
// spoofed host match on their own request host (the decision is keyed
// on the destination that sniffs the UA).  Fail-open like strippedUrl/
// isDomainBlocked: an FFI hiccup or a no-rust build sends the profile
// UA, never a block.  Clearnet only — TorRequestInterceptor never
// consults the table: a per-site UA on a tor page would re-identify
// the session the uniform-UA pin exists to protect.
void PrivacyRequestInterceptor::applyUserAgentSpoof(
        QWebEngineUrlRequestInfo &info)
{
#if defined(ARKTOS_RUSTCORE)
    const QString host = info.requestUrl().host().toLower();
    if (host.isEmpty())
        return;
    char *out = rc_ua_spoof(host.toUtf8().constData());
    if (!out)
        return;
    const QByteArray ua(out);
    rc_string_free(out);
    if (!ua.isEmpty())
        info.setHttpHeader("User-Agent", ua);
#else
    Q_UNUSED(info);
#endif
}
