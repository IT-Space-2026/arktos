/*
 * Copyright © 2026-2027 Arktos Browser - IT-Space
 * Copyright 2008-2009 Arora Browser
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

/****************************************************************************
**
** Copyright (C) 2007-2008 Trolltech ASA. All rights reserved.
**
** This file is part of the demonstration applications of the Qt Toolkit.
**
** This file may be used under the terms of the GNU General Public
** License versions 2.0 or 3.0 as published by the Free Software
** Foundation and appearing in the files LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file.  Alternatively you may (at
** your option) use any later version of the GNU General Public
** License if such license has been publicly approved by Trolltech ASA
** (or its successors, if any) and the KDE Free Qt Foundation. In
** addition, as a special exception, Trolltech gives you certain
** additional rights. These rights are described in the Trolltech GPL
** Exception version 1.2, which can be found at
** http://trolltech.com/products/qt/gplexception/ and in the file
** GPL_EXCEPTION.txt in this package.
**
** Please review the following information to ensure GNU General
** Public Licensing requirements will be met:
** http://trolltech.com/products/qt/licenses/licensing/opensource/. If
** you are unsure which license is appropriate for your use, please
** review the following information:
** http://trolltech.com/products/qt/licenses/licensing/licensingoverview
** or contact the sales department at sales@trolltech.com.
**
** In addition, as a special exception, Trolltech, as the sole
** copyright holder for Qt Designer, grants users of the Qt/Eclipse
** Integration plug-in the right for the Qt/Eclipse Integration to
** link to functionality provided by Qt Designer and its related
** libraries.
**
** This file is provided "AS IS" with NO WARRANTY OF ANY KIND,
** INCLUDING THE WARRANTIES OF DESIGN, MERCHANTABILITY AND FITNESS FOR
** A PARTICULAR PURPOSE.  See the GNU General Public License for more
** details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor,
** Boston, MA  02110-1301  USA
**
****************************************************************************/

#include "cookiejar.h"
#include "hostmatch.h"
#include "profilesettings.h"

#include "autosaver.h"
#include "browserprofile.h"
#include "containermanager.h"
#include "navigationpolicy.h"

#include <qcoreapplication.h>
#include <qdatetime.h>
#include <qhash.h>
#include <qmetaobject.h>
#include <qnetworkcookiejar.h>
#include <qsettings.h>
#include <qwebenginecookiestore.h>
#include <qwebengineprofile.h>

#if defined(ARKTOS_RUSTCORE)
#include "rustcorebridge.h"
#include "sitedecisionstore.h"
#include <arktos_core.h>
#include <rustcore.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#endif

#include <algorithm>

#include <qdebug.h>

CookieJar::CookieJar(QWebEngineProfile *profile, QObject *parent)
    : QObject(parent)
    , m_profile(profile ? profile : QWebEngineProfile::defaultProfile())
    , m_profileId(BrowserProfile::profileIdFor(m_profile))
    , m_offTheRecord(m_profile->isOffTheRecord())
    , m_store(m_profile->cookieStore())
    , m_saveTimer(new AutoSaver(this))
    , m_filterTrackingCookies(false)
    , m_blockThirdPartyCookies(true)
    , m_acceptCookies(AcceptOnlyFromSitesNavigatedTo)
    , m_keepCookies(KeepUntilExpire)
    , m_sessionLength(-1)
{
    loadSettings();

    // The filter callback runs on the browser IO thread; it only reads
    // the lock-guarded policy snapshot.  ENG02: the accept/reject
    // decision itself is delegated to Engine::NavigationPolicy — the
    // jar only marshals its rule lists in.
    m_store->setCookieFilter([this](const QWebEngineCookieStore::FilterRequest &request) {
        const QReadLocker lock(&m_policyLock);
        Engine::CookieGateInput input;
        input.host = request.origin.host();
        if (input.host.isEmpty())
            input.host = request.firstPartyUrl.host();
        input.thirdParty = request.thirdParty;
        input.blockThirdParty = m_policy.blockThirdPartyCookies;
        input.acceptPolicy = static_cast<int>(m_policy.acceptCookies);
        input.block = m_policy.block;
        input.allow = m_policy.allow;
        input.allowForSession = m_policy.allowForSession;
        return Engine::NavigationPolicy::cookieFilter(input);
    });

    connect(m_store, &QWebEngineCookieStore::cookieAdded,
            this, &CookieJar::handleCookieAdded);
    connect(m_store, &QWebEngineCookieStore::cookieRemoved,
            this, &CookieJar::handleCookieRemoved);
    m_store->loadAllCookies();
}

CookieJar::CookieJar(QObject *parent)
    : CookieJar(nullptr, parent)
{
}

static QHash<QWebEngineProfile *, CookieJar *> &cookieJars()
{
    static QHash<QWebEngineProfile *, CookieJar *> jars;
    return jars;
}

CookieJar *CookieJar::instance(QWebEngineProfile *profile)
{
    if (!profile)
        profile = BrowserProfile::normalProfile();
    CookieJar *&jar = cookieJars()[profile];
    if (!jar) {
        jar = new CookieJar(profile, qApp);
        // A destroyed profile must not leave a jar keyed on its dead
        // address — a recycled profile object would inherit the stale
        // jar (MEM10, same contract as the OTR notes map).  ~CookieJar
        // only touches the QPointer-guarded store and the cached
        // m_offTheRecord flag — never the dying profile — so running
        // it from the destroyed signal is safe (CA10 L1).
        QObject::connect(profile, &QObject::destroyed, qApp, [profile]() {
            delete cookieJars().take(profile);
        });
    }
    return jar;
}

CookieJar *CookieJar::instanceFor(const QString &profileId)
{
    return instance(BrowserProfile::profileFor(profileId));
}

void CookieJar::releaseForProfile(QWebEngineProfile *profile)
{
    delete cookieJars().take(profile);
}

void CookieJar::releaseFor(const QString &profileId)
{
    // Jars key on the profile OBJECT: the persona's named profile plus
    // every container profile its ContainerManager materialized.
    // Runs before either profile dies — ~CookieJar still touches the
    // profile's cookie store through the QPointer guard.
    releaseForProfile(BrowserProfile::personaProfileIfCreated(profileId));
    if (ContainerManager *manager =
            ContainerManager::instanceIfCreated(profileId)) {
        const QList<QWebEngineProfile *> containers =
            manager->createdProfiles();
        for (QWebEngineProfile *profile : containers)
            releaseForProfile(profile);
    }
}

CookieJar::~CookieJar()
{
    // The store can already be gone during WebEngine teardown; the
    // QPointer guard keeps this from dereferencing a dead object.
    if (m_store)
        m_store->setCookieFilter(nullptr);
    m_saveTimer->saveIfNeccessary();
}

QWebEngineProfile *CookieJar::profile() const
{
    return m_profile;
}

bool CookieJar::isPrivate() const
{
    // Qt WebEngine has no runtime private-browsing toggle: privacy is a
    // property of the profile.  A private CookieJar is one bound to an
    // off-the-record profile (see BrowserApplication::webEngineProfile).
    return m_offTheRecord;
}

#if defined(ARKTOS_RUSTCORE)
namespace {

// MD23: QNetworkCookie -> the RC15 record JSON for this jar's
// container partition.  The jar keeps the Set-Cookie Domain
// verbatim: a leading dot marks a domain cookie, its absence a
// host-exact one — the same split the RC15 hostOnly flag records
// (cookies.rs documents the measured Qt behavior).
QJsonObject rustCookieJson(const QNetworkCookie &cookie,
                         const QString &container)
{
    QString domain = cookie.domain();
    const bool hostOnly = !domain.startsWith(QLatin1Char('.'));
    if (!hostOnly)
        domain = domain.mid(1);
    QJsonObject o;
    o.insert(QLatin1String("container"), container);
    o.insert(QLatin1String("name"),
             QString::fromUtf8(cookie.name()));
    o.insert(QLatin1String("value"),
             QString::fromUtf8(cookie.value()));
    o.insert(QLatin1String("domain"), domain);
    o.insert(QLatin1String("path"), cookie.path().isEmpty()
             ? QStringLiteral("/") : cookie.path());
    if (cookie.isSessionCookie())
        o.insert(QLatin1String("expires"), QJsonValue());
    else
        o.insert(QLatin1String("expires"),
                 double(cookie.expirationDate().toMSecsSinceEpoch()));
    o.insert(QLatin1String("secure"), cookie.isSecure());
    o.insert(QLatin1String("httpOnly"), cookie.isHttpOnly());
    o.insert(QLatin1String("sameSite"), int(cookie.sameSitePolicy()));
    o.insert(QLatin1String("hostOnly"), hostOnly);
    o.insert(QLatin1String("created"),
             double(QDateTime::currentMSecsSinceEpoch()));
    return o;
}

} // namespace

AcCookies *CookieJar::rustCookies() const
{
    // A private jar never mirrors — the persistent db must not see
    // off-the-record cookies (the same disk silence save() keeps).
    if (m_offTheRecord)
        return nullptr;
    // Same drift rule HistoryManager::rustStore() documents: a
    // minted handle pins the root it was bound against, so a
    // test-mode data-root move drops it for a re-mint.
    if (m_rustCookies && RustCoreBridge::rootDrifted()) {
        ac_cookies_close(m_rustCookies);
        m_rustCookies = nullptr;
        m_rustRoleDeclared = false;
    }
    if (!m_rustCookies) {
        if (RustCoreBridge *bridge = RustCoreBridge::serving())
            m_rustCookies = bridge->cookies(m_profileId);
    }
    if (!m_rustCookies)
        ensureRustCookies();
    // The declared two-mode contract (cookies.rs): on the Chromium
    // lane the engine owns the jar and this db is its durable shadow
    // — the persisted meta flag is how the db honestly reports it.
    if (!m_rustRoleDeclared) {
        const AcStatus st = m_rustCookies
            ? ac_cookies_set_role(m_rustCookies, AC_COOKIES_MIRROR)
            : rc_cookies_set_role(AC_COOKIES_MIRROR);
        m_rustRoleDeclared = (st == AC_OK);
    }
    return m_rustCookies;
}

void CookieJar::ensureRustCookies() const
{
    // Legacy-lane anchor: the process-global store re-binds to this
    // profile's cookies.db (a no-op re-assert when already open).
    rustCoreEnsureDataDirFor(m_profileId);
    rc_cookies_open(nullptr);
}

void CookieJar::rustCookieSet(const QNetworkCookie &cookie) const
{
    if (m_offTheRecord)
        return;
    QString container;
    // ContainerManager::containerIdForProfile resolves the normal
    // profile to "" — the store's default partition.
    if (ContainerManager *manager =
            ContainerManager::instanceIfCreated(m_profileId))
        container = manager->containerIdForProfile(m_profile);
    const QByteArray json = QJsonDocument(
        rustCookieJson(cookie, container)).toJson(QJsonDocument::Compact);
    if (AcCookies *h = rustCookies()) {
        ac_cookies_set(h,
                       reinterpret_cast<const uint8_t *>(json.constData()),
                       size_t(json.size()));
    } else {
        rc_cookies_set(
            reinterpret_cast<const uint8_t *>(json.constData()),
            size_t(json.size()));
    }
}

void CookieJar::rustCookieRemove(const QNetworkCookie &cookie) const
{
    if (m_offTheRecord)
        return;
    QString container;
    if (ContainerManager *manager =
            ContainerManager::instanceIfCreated(m_profileId))
        container = manager->containerIdForProfile(m_profile);
    QString domain = cookie.domain();
    if (domain.startsWith(QLatin1Char('.')))
        domain = domain.mid(1);
    const QByteArray d = domain.toUtf8();
    const QByteArray n = cookie.name();
    const QByteArray p = (cookie.path().isEmpty()
                          ? QStringLiteral("/") : cookie.path()).toUtf8();
    // Explicit "" container — the default partition.  NULL would
    // also resolve to default on remove, but "" is unambiguous.
    const QByteArray c = container.toUtf8();
    if (AcCookies *h = rustCookies())
        ac_cookies_remove(h, d.constData(), n.constData(), p.constData(),
                          c.constData());
    else
        rc_cookies_remove(d.constData(), n.constData(), p.constData(),
                          c.constData());
}

void CookieJar::rustCookiesClear() const
{
    if (m_offTheRecord)
        return;
    QString container;
    if (ContainerManager *manager =
            ContainerManager::instanceIfCreated(m_profileId))
        container = manager->containerIdForProfile(m_profile);
    // Explicit "" — the default partition ONLY.  NULL means "wipe
    // the whole db" and would erase container partitions owned by
    // other jars.
    const QByteArray c = container.toUtf8();
    if (AcCookies *h = rustCookies())
        ac_cookies_clear(h, c.constData());
    else
        rc_cookies_clear(c.constData());
}
#endif // ARKTOS_RUSTCORE

void CookieJar::clear()
{
    m_cookies.clear();
    if (m_store)
        m_store->deleteAllCookies();
#if defined(ARKTOS_RUSTCORE)
    // deleteAllCookies is a bulk op — the per-cookie removed signals
    // are not guaranteed, so the mirror drops the partition itself.
    rustCookiesClear();
#endif
    emit cookiesChanged();
}

void CookieJar::loadSettings()
{
    ProfileSettings settings(m_profileId);
    settings.beginGroup(QLatin1String("cookies"));
    QByteArray value = settings.value(QLatin1String("acceptCookies"),
                                      QLatin1String("AcceptOnlyFromSitesNavigatedTo")).toByteArray();
    QMetaEnum acceptPolicyEnum = staticMetaObject.enumerator(staticMetaObject.indexOfEnumerator("AcceptPolicy"));
    m_acceptCookies = acceptPolicyEnum.keyToValue(value) == -1 ?
                      AcceptOnlyFromSitesNavigatedTo :
                      static_cast<AcceptPolicy>(acceptPolicyEnum.keyToValue(value));

    value = settings.value(QLatin1String("keepCookiesUntil"), QLatin1String("KeepUntilExpire")).toByteArray();
    QMetaEnum keepPolicyEnum = staticMetaObject.enumerator(staticMetaObject.indexOfEnumerator("KeepPolicy"));
    m_keepCookies = keepPolicyEnum.keyToValue(value) == -1 ?
                    KeepUntilExpire :
                    static_cast<KeepPolicy>(keepPolicyEnum.keyToValue(value));

    m_filterTrackingCookies = settings.value(QLatin1String("filterTrackingCookies"), m_filterTrackingCookies).toBool();
    m_blockThirdPartyCookies = settings.value(QLatin1String("blockThirdPartyCookies"), m_blockThirdPartyCookies).toBool();
    m_sessionLength = settings.value(QLatin1String("sessionLength"), -1).toInt();

    settings.beginGroup(QLatin1String("exceptions"));
#if defined(ARKTOS_RUSTCORE)
    // SITED01: one-shot import — replay the three legacy exception
    // lists into the Rust store ("block"/"allow"/"session" values),
    // then retire the group after the writes land.
    const QStringList legacyBlock =
        settings.value(QLatin1String("block")).toStringList();
    const QStringList legacyAllow =
        settings.value(QLatin1String("allow")).toStringList();
    const QStringList legacySession =
        settings.value(QLatin1String("allowForSession")).toStringList();
    if (!legacyBlock.isEmpty() || !legacyAllow.isEmpty()
        || !legacySession.isEmpty()) {
        bool allWritten = true;
        for (const QString &host : legacySession) {
            if (!host.isEmpty())
                allWritten = SiteDecisionStore::set(
                    m_profileId,
                    SiteDecisionStore::KindCookie,
                    host, QLatin1String("session")) && allWritten;
        }
        for (const QString &host : legacyAllow) {
            if (!host.isEmpty())
                allWritten = SiteDecisionStore::set(
                    m_profileId,
                    SiteDecisionStore::KindCookie,
                    host, QLatin1String("allow")) && allWritten;
        }
        // Block is written last: a host on multiple lists resolves
        // to deny, the same precedence the filter applies.
        for (const QString &host : legacyBlock) {
            if (!host.isEmpty())
                allWritten = SiteDecisionStore::set(
                    m_profileId,
                    SiteDecisionStore::KindCookie,
                    host, QLatin1String("block")) && allWritten;
        }
        if (allWritten) {
            settings.remove(QLatin1String("block"));
            settings.remove(QLatin1String("allow"));
            settings.remove(QLatin1String("allowForSession"));
        }
    }
    settings.endGroup();
    settings.endGroup();
    m_exceptions_block.clear();
    m_exceptions_allow.clear();
    m_exceptions_allowForSession.clear();
    const QHash<QString, QString> rows =
        SiteDecisionStore::entries(m_profileId,
                                   SiteDecisionStore::KindCookie);
    for (auto it = rows.constBegin(); it != rows.constEnd(); ++it) {
        if (it.value() == QLatin1String("block"))
            m_exceptions_block.append(it.key());
        else if (it.value() == QLatin1String("allow"))
            m_exceptions_allow.append(it.key());
        else if (it.value() == QLatin1String("session"))
            m_exceptions_allowForSession.append(it.key());
        // Unknown values are corrupt rows — dropped on load.
    }
#else
    m_exceptions_block = settings.value(QLatin1String("block")).toStringList();
    m_exceptions_allow = settings.value(QLatin1String("allow")).toStringList();
    m_exceptions_allowForSession = settings.value(QLatin1String("allowForSession")).toStringList();
    settings.endGroup();
    settings.endGroup();
#endif

    std::sort(m_exceptions_block.begin(), m_exceptions_block.end());
    std::sort(m_exceptions_allow.begin(), m_exceptions_allow.end());
    std::sort(m_exceptions_allowForSession.begin(), m_exceptions_allowForSession.end());

    applyKeepPolicy();
    updatePolicySnapshot();
    emit cookiesChanged();
}

void CookieJar::save()
{
    // The cookies themselves are persisted by the WebEngine profile
    // (setPersistentCookiesPolicy); only the policy and the exception
    // lists are ours to store.  An off-the-record jar never writes
    // either (MPROF02).
    if (m_offTheRecord)
        return;
    ProfileSettings settings(m_profileId);
    settings.beginGroup(QLatin1String("cookies"));

    QMetaEnum acceptPolicyEnum = staticMetaObject.enumerator(staticMetaObject.indexOfEnumerator("AcceptPolicy"));
    settings.setValue(QLatin1String("acceptCookies"), QLatin1String(acceptPolicyEnum.valueToKey(m_acceptCookies)));

    QMetaEnum keepPolicyEnum = staticMetaObject.enumerator(staticMetaObject.indexOfEnumerator("KeepPolicy"));
    settings.setValue(QLatin1String("keepCookiesUntil"), QLatin1String(keepPolicyEnum.valueToKey(m_keepCookies)));

    settings.setValue(QLatin1String("filterTrackingCookies"), m_filterTrackingCookies);
    settings.setValue(QLatin1String("blockThirdPartyCookies"), m_blockThirdPartyCookies);
    settings.setValue(QLatin1String("sessionLength"), m_sessionLength);

#if defined(ARKTOS_RUSTCORE)
    // The three lists mirror into one kind — merged with the same
    // precedence the filter resolves (block > allow > session).
    QHash<QString, QString> rows;
    for (const QString &host : m_exceptions_allowForSession)
        rows.insert(host, QLatin1String("session"));
    for (const QString &host : m_exceptions_allow)
        rows.insert(host, QLatin1String("allow"));
    for (const QString &host : m_exceptions_block)
        rows.insert(host, QLatin1String("block"));
    SiteDecisionStore::replace(m_profileId,
                               SiteDecisionStore::KindCookie, rows);
#else
    settings.beginGroup(QLatin1String("exceptions"));
    settings.setValue(QLatin1String("block"), m_exceptions_block);
    settings.setValue(QLatin1String("allow"), m_exceptions_allow);
    settings.setValue(QLatin1String("allowForSession"), m_exceptions_allowForSession);
#endif
}

void CookieJar::updatePolicySnapshot()
{
    QWriteLocker lock(&m_policyLock);
    m_policy.acceptCookies = m_acceptCookies;
    m_policy.filterTrackingCookies = m_filterTrackingCookies;
    m_policy.blockThirdPartyCookies = m_blockThirdPartyCookies;
    m_policy.block = m_exceptions_block;
    m_policy.allow = m_exceptions_allow;
    m_policy.allowForSession = m_exceptions_allowForSession;
}

void CookieJar::applyKeepPolicy()
{
    if (m_offTheRecord)
        return;
    m_profile->setPersistentCookiesPolicy(m_keepCookies == KeepUntilExit
            ? QWebEngineProfile::NoPersistentCookies
            : QWebEngineProfile::AllowPersistentCookies);
}

void CookieJar::handleCookieAdded(const QNetworkCookie &cookie)
{
    if (!m_store)
        return;
    for (int i = m_cookies.count() - 1; i >= 0; --i) {
        const QNetworkCookie &old = m_cookies.at(i);
        if (old.name() == cookie.name()
            && old.domain() == cookie.domain()
            && old.path() == cookie.path())
            m_cookies.removeAt(i);
    }
    m_cookies.append(cookie);

    // FilterRequest carries no cookie object in Qt 6.11, so rules that
    // need the cookie itself run here as a post-add rewrite/delete.
    QNetworkCookie rewritten = cookie;
    bool rewrite = false;
    const QString domain = cookie.domain();
    if (m_filterTrackingCookies && cookie.name().startsWith("__utm")) {
        m_cookies.removeLast();
        m_store->deleteCookie(cookie);
        emit cookiesChanged();
        return;
    }
    if (!cookie.isSessionCookie()
        && isOnDomainList(m_exceptions_allowForSession, domain)) {
        rewritten.setExpirationDate(QDateTime());
        rewrite = true;
    } else if (cookie.isSessionCookie() && m_sessionLength != -1) {
        rewritten.setExpirationDate(QDateTime::currentDateTime().addDays(m_sessionLength));
        rewrite = true;
    }
    if (m_keepCookies == KeepUntilTimeLimit && !rewritten.isSessionCookie()) {
        QDateTime soon = QDateTime::currentDateTime().addDays(90);
        if (rewritten.expirationDate() > soon) {
            rewritten.setExpirationDate(soon);
            rewrite = true;
        }
    }
    if (rewrite) {
        // setCookie() bypasses the filter, so this cannot recurse
        // through acceptCookie; the rewritten cookie re-enters via
        // cookieAdded and already satisfies every rule.
        m_cookies.removeLast();
        m_cookies.append(rewritten);
        m_store->setCookie(rewritten);
    }
#if defined(ARKTOS_RUSTCORE)
    // MD23: the RC15 mirror tracks what the jar actually kept — the
    // rewritten form when a rule rewrote it (its own cookieAdded
    // re-entry lands the same upsert, idempotent).
    rustCookieSet(rewrite ? rewritten : cookie);
#endif
    emit cookiesChanged();
}

void CookieJar::handleCookieRemoved(const QNetworkCookie &cookie)
{
    for (int i = m_cookies.count() - 1; i >= 0; --i) {
        const QNetworkCookie &old = m_cookies.at(i);
        if (old.name() == cookie.name()
            && old.domain() == cookie.domain()
            && old.path() == cookie.path())
            m_cookies.removeAt(i);
    }
#if defined(ARKTOS_RUSTCORE)
    rustCookieRemove(cookie);
#endif
    emit cookiesChanged();
}

namespace {
// QNetworkCookieJar already implements the canonical domain/path
// matching; this just exposes setAllCookies() so the mirror can feed it.
class MirrorCookieJar : public QNetworkCookieJar
{
public:
    using QNetworkCookieJar::setAllCookies;
};
}

QList<QNetworkCookie> CookieJar::cookiesForUrl(const QUrl &url) const
{
    MirrorCookieJar jar;
    jar.setAllCookies(m_cookies);
    return jar.cookiesForUrl(url);
}

bool CookieJar::isAllowedForHost(const QString &host, bool thirdParty) const
{
    // ENG02: same delegated gate as the IO-thread store filter.
    Engine::CookieGateInput input;
    input.host = host;
    input.thirdParty = thirdParty;
    input.blockThirdParty = m_blockThirdPartyCookies;
    input.acceptPolicy = static_cast<int>(m_acceptCookies);
    input.block = m_exceptions_block;
    input.allow = m_exceptions_allow;
    input.allowForSession = m_exceptions_allowForSession;
    return Engine::NavigationPolicy::cookieFilter(input);
}

bool CookieJar::setCookiesFromUrl(const QList<QNetworkCookie> &cookieList, const QUrl &url)
{
    // App-side entry point: QWebEngineCookieStore::setCookie() does not
    // run the cookie filter, so the policy is applied here explicitly.
    // Cookies pushed by web pages go through the filter instead.
    QString host = url.host();
    if (!m_store || !isAllowedForHost(host, false))
        return false;

    bool addedCookies = false;
    QDateTime soon = QDateTime::currentDateTime().addDays(90);
    for (QNetworkCookie cookie : cookieList) {
        if (cookie.isSessionCookie() && m_sessionLength != -1)
            cookie.setExpirationDate(QDateTime::currentDateTime().addDays(m_sessionLength));
        if (m_filterTrackingCookies && cookie.name().startsWith("__utm"))
            continue;
        if (isOnDomainList(m_exceptions_allowForSession, host))
            cookie.setExpirationDate(QDateTime());
        if (m_keepCookies == KeepUntilTimeLimit
            && !cookie.isSessionCookie()
            && cookie.expirationDate() > soon)
            cookie.setExpirationDate(soon);
        m_store->setCookie(cookie, url);
        addedCookies = true;
    }
    if (addedCookies)
        m_saveTimer->changeOccurred();
    return addedCookies;
}

QList<QNetworkCookie> CookieJar::cookies() const
{
    return m_cookies;
}

void CookieJar::setCookies(const QList<QNetworkCookie> &cookies)
{
    // Reconcile the store with the given list; the mirror is updated
    // immediately and confirmed by the store's added/removed signals.
    const QList<QNetworkCookie> old = m_cookies;
    if (m_store) {
        for (const QNetworkCookie &cookie : old) {
            if (!cookies.contains(cookie))
                m_store->deleteCookie(cookie);
        }
        for (const QNetworkCookie &cookie : cookies) {
            if (!old.contains(cookie))
                m_store->setCookie(cookie);
        }
    }
    m_cookies = cookies;
    m_saveTimer->changeOccurred();
    emit cookiesChanged();
}

bool CookieJar::ruleForHost(const QString &host, CookieRule *rule) const
{
    // Same precedence the IO-thread filter applies: block first, then
    // allow, then allow-for-session.
    if (isOnDomainList(m_exceptions_block, host)) {
        if (rule)
            *rule = Block;
        return true;
    }
    if (isOnDomainList(m_exceptions_allow, host)) {
        if (rule)
            *rule = Allow;
        return true;
    }
    if (isOnDomainList(m_exceptions_allowForSession, host)) {
        if (rule)
            *rule = AllowForSession;
        return true;
    }
    return false;
}

// Entries targeting exactly this host — "host" and its ".host" twin.
// Broader rules ("example.com" covering "www.example.com") are left
// alone: dropping them from here would unblock the whole domain.
static QStringList withoutHostEntries(QStringList list, const QString &host)
{
    const QString dotted = QLatin1Char('.') + host;
    list.removeAll(host);
    list.removeAll(dotted);
    return list;
}

void CookieJar::clearRuleForHost(const QString &host)
{
    if (host.isEmpty())
        return;
    setBlockedCookies(withoutHostEntries(m_exceptions_block, host));
    setAllowedCookies(withoutHostEntries(m_exceptions_allow, host));
    setAllowForSessionCookies(
        withoutHostEntries(m_exceptions_allowForSession, host));
}

void CookieJar::setRuleForHost(const QString &host, CookieRule rule)
{
    if (host.isEmpty())
        return;
    clearRuleForHost(host);
    switch (rule) {
    case Block:
        setBlockedCookies(withoutHostEntries(m_exceptions_block, host)
                          << host);
        break;
    case Allow:
        setAllowedCookies(withoutHostEntries(m_exceptions_allow, host)
                          << host);
        break;
    case AllowForSession:
        setAllowForSessionCookies(
            withoutHostEntries(m_exceptions_allowForSession, host)
            << host);
        break;
    }
}

void CookieJar::removeCookiesForHost(const QString &host)
{
    if (host.isEmpty())
        return;
    QList<QNetworkCookie> keep;
    bool removed = false;
    for (const QNetworkCookie &cookie : m_cookies) {
        QString domain = cookie.domain();
        if (domain.startsWith(QLatin1Char('.')))
            domain = domain.mid(1);
        // Scoped to the host or a subdomain the site controls...
        const bool underHost = domain == host
            || domain.endsWith(QLatin1Char('.') + host);
        // ...or to a parent domain the site still receives.
        const bool coversHost = host == domain
            || host.endsWith(QLatin1Char('.') + domain);
        if (underHost || coversHost) {
            if (m_store)
                m_store->deleteCookie(cookie);
            removed = true;
        } else {
            keep.append(cookie);
        }
    }
    if (removed) {
        m_cookies = keep;
        m_saveTimer->changeOccurred();
        emit cookiesChanged();
    }
}

bool CookieJar::isOnDomainList(const QStringList &rules, const QString &domain)
{
    // Either the rule matches the domain exactly
    // or the domain ends with ".rule"
    return HostMatch::isOnDomainList(rules, domain);
}

CookieJar::AcceptPolicy CookieJar::acceptPolicy() const
{
    return m_acceptCookies;
}

void CookieJar::setAcceptPolicy(AcceptPolicy policy)
{
    if (policy == m_acceptCookies)
        return;
    m_acceptCookies = policy;
    updatePolicySnapshot();
    m_saveTimer->changeOccurred();
}

CookieJar::KeepPolicy CookieJar::keepPolicy() const
{
    return m_keepCookies;
}

void CookieJar::setKeepPolicy(KeepPolicy policy)
{
    if (policy == m_keepCookies)
        return;
    m_keepCookies = policy;
    applyKeepPolicy();
    updatePolicySnapshot();
    m_saveTimer->changeOccurred();
}

QStringList CookieJar::blockedCookies() const
{
    return m_exceptions_block;
}

QStringList CookieJar::allowedCookies() const
{
    return m_exceptions_allow;
}

QStringList CookieJar::allowForSessionCookies() const
{
    return m_exceptions_allowForSession;
}

void CookieJar::setBlockedCookies(QStringList list)
{
    m_exceptions_block = std::move(list);
    std::sort(m_exceptions_block.begin(), m_exceptions_block.end());
    updatePolicySnapshot();
    applyRules();
    m_saveTimer->changeOccurred();
}

void CookieJar::setAllowedCookies(QStringList list)
{
    m_exceptions_allow = std::move(list);
    std::sort(m_exceptions_allow.begin(), m_exceptions_allow.end());
    updatePolicySnapshot();
    applyRules();
    m_saveTimer->changeOccurred();
}

void CookieJar::setAllowForSessionCookies(QStringList list)
{
    m_exceptions_allowForSession = std::move(list);
    std::sort(m_exceptions_allowForSession.begin(), m_exceptions_allowForSession.end());
    updatePolicySnapshot();
    applyRules();
    m_saveTimer->changeOccurred();
}

void CookieJar::applyRules()
{
    if (!m_store)
        return;
    for (const QNetworkCookie &cookie : m_cookies) {
        if (isOnDomainList(m_exceptions_block, cookie.domain())) {
            m_store->deleteCookie(cookie);
        } else if (!cookie.isSessionCookie()
                   && isOnDomainList(m_exceptions_allowForSession, cookie.domain())) {
            QNetworkCookie rewritten = cookie;
            rewritten.setExpirationDate(QDateTime());
            m_store->setCookie(rewritten);
        }
    }
}

bool CookieJar::blockThirdPartyCookies() const
{
    return m_blockThirdPartyCookies;
}

void CookieJar::setBlockThirdPartyCookies(bool blockThirdPartyCookies)
{
    m_blockThirdPartyCookies = blockThirdPartyCookies;
    updatePolicySnapshot();
}
