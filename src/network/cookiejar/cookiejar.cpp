/*
 * Copyright 2008-2009 Benjamin C. Meyer <ben@meyerhome.net>
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

#include "autosaver.h"
#include "browserprofile.h"

#include <qcoreapplication.h>
#include <qdatetime.h>
#include <qhash.h>
#include <qmetaobject.h>
#include <qnetworkcookiejar.h>
#include <qsettings.h>
#include <qwebenginecookiestore.h>
#include <qwebengineprofile.h>

#include <algorithm>

#include <qdebug.h>

CookieJar::CookieJar(QWebEngineProfile *profile, QObject *parent)
    : QObject(parent)
    , m_profile(profile ? profile : QWebEngineProfile::defaultProfile())
    , m_store(m_profile->cookieStore())
    , m_saveTimer(new AutoSaver(this))
    , m_filterTrackingCookies(false)
    , m_acceptCookies(AcceptOnlyFromSitesNavigatedTo)
    , m_keepCookies(KeepUntilExpire)
    , m_sessionLength(-1)
{
    loadSettings();

    // The filter callback runs on the browser IO thread; it only reads
    // the lock-guarded policy snapshot.
    m_store->setCookieFilter([this](const QWebEngineCookieStore::FilterRequest &request) {
        QReadLocker lock(&m_policyLock);
        QString host = request.origin.host();
        if (host.isEmpty())
            host = request.firstPartyUrl.host();
        bool block = isOnDomainList(m_policy.block, host);
        bool allow = !block && isOnDomainList(m_policy.allow, host);
        bool allowForSession = !block && !allow && isOnDomainList(m_policy.allowForSession, host);
        if (block)
            return false;
        switch (m_policy.acceptCookies) {
        case AcceptAlways:
            return true;
        case AcceptNever:
            return allow || allowForSession;
        case AcceptOnlyFromSitesNavigatedTo:
        default:
            return allow || allowForSession || !request.thirdParty;
        }
    });

    connect(m_store, &QWebEngineCookieStore::cookieAdded,
            this, &CookieJar::handleCookieAdded);
    connect(m_store, &QWebEngineCookieStore::cookieRemoved,
            this, &CookieJar::handleCookieRemoved);
    m_store->loadAllCookies();
}

CookieJar::CookieJar(QObject *parent)
    : CookieJar(0, parent)
{
}

CookieJar *CookieJar::instance(QWebEngineProfile *profile)
{
    if (!profile)
        profile = BrowserProfile::normalProfile();
    static QHash<QWebEngineProfile*, CookieJar*> jars;
    CookieJar *&jar = jars[profile];
    if (!jar)
        jar = new CookieJar(profile, qApp);
    return jar;
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
    return m_profile->isOffTheRecord();
}

void CookieJar::clear()
{
    m_cookies.clear();
    if (m_store)
        m_store->deleteAllCookies();
    emit cookiesChanged();
}

void CookieJar::loadSettings()
{
    QSettings settings;
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
    m_sessionLength = settings.value(QLatin1String("sessionLength"), -1).toInt();

    settings.beginGroup(QLatin1String("exceptions"));
    m_exceptions_block = settings.value(QLatin1String("block")).toStringList();
    m_exceptions_allow = settings.value(QLatin1String("allow")).toStringList();
    m_exceptions_allowForSession = settings.value(QLatin1String("allowForSession")).toStringList();
    settings.endGroup();
    settings.endGroup();

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
    // lists are ours to store.
    QSettings settings;
    settings.beginGroup(QLatin1String("cookies"));

    QMetaEnum acceptPolicyEnum = staticMetaObject.enumerator(staticMetaObject.indexOfEnumerator("AcceptPolicy"));
    settings.setValue(QLatin1String("acceptCookies"), QLatin1String(acceptPolicyEnum.valueToKey(m_acceptCookies)));

    QMetaEnum keepPolicyEnum = staticMetaObject.enumerator(staticMetaObject.indexOfEnumerator("KeepPolicy"));
    settings.setValue(QLatin1String("keepCookiesUntil"), QLatin1String(keepPolicyEnum.valueToKey(m_keepCookies)));

    settings.setValue(QLatin1String("filterTrackingCookies"), m_filterTrackingCookies);
    settings.setValue(QLatin1String("sessionLength"), m_sessionLength);

    settings.beginGroup(QLatin1String("exceptions"));
    settings.setValue(QLatin1String("block"), m_exceptions_block);
    settings.setValue(QLatin1String("allow"), m_exceptions_allow);
    settings.setValue(QLatin1String("allowForSession"), m_exceptions_allowForSession);
}

void CookieJar::updatePolicySnapshot()
{
    QWriteLocker lock(&m_policyLock);
    m_policy.acceptCookies = m_acceptCookies;
    m_policy.filterTrackingCookies = m_filterTrackingCookies;
    m_policy.block = m_exceptions_block;
    m_policy.allow = m_exceptions_allow;
    m_policy.allowForSession = m_exceptions_allowForSession;
}

void CookieJar::applyKeepPolicy()
{
    if (m_profile->isOffTheRecord())
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
    bool block = isOnDomainList(m_exceptions_block, host);
    bool allow = !block && isOnDomainList(m_exceptions_allow, host);
    bool allowForSession = !block && !allow && isOnDomainList(m_exceptions_allowForSession, host);
    if (block)
        return false;
    switch (m_acceptCookies) {
    case AcceptAlways:
        return true;
    case AcceptNever:
        return allow || allowForSession;
    case AcceptOnlyFromSitesNavigatedTo:
    default:
        return allow || allowForSession || !thirdParty;
    }
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
    foreach (QNetworkCookie cookie, cookieList) {
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
        foreach (const QNetworkCookie &cookie, old) {
            if (!cookies.contains(cookie))
                m_store->deleteCookie(cookie);
        }
        foreach (const QNetworkCookie &cookie, cookies) {
            if (!old.contains(cookie))
                m_store->setCookie(cookie);
        }
    }
    m_cookies = cookies;
    m_saveTimer->changeOccurred();
    emit cookiesChanged();
}

bool CookieJar::isOnDomainList(const QStringList &rules, const QString &domain)
{
    // Either the rule matches the domain exactly
    // or the domain ends with ".rule"
    foreach (const QString &rule, rules) {
        if (rule.startsWith(QLatin1String("."))) {
            if (domain.endsWith(rule))
                return true;

            QStringView withoutDot = QStringView(rule).right(rule.size() - 1);
            if (domain == withoutDot)
                return true;
        } else {
            if (domain.endsWith(QLatin1Char('.') + rule))
                return true;

            if (rule == domain)
                return true;
        }
    }
    return false;
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

void CookieJar::setBlockedCookies(const QStringList &list)
{
    m_exceptions_block = list;
    std::sort(m_exceptions_block.begin(), m_exceptions_block.end());
    updatePolicySnapshot();
    applyRules();
    m_saveTimer->changeOccurred();
}

void CookieJar::setAllowedCookies(const QStringList &list)
{
    m_exceptions_allow = list;
    std::sort(m_exceptions_allow.begin(), m_exceptions_allow.end());
    updatePolicySnapshot();
    applyRules();
    m_saveTimer->changeOccurred();
}

void CookieJar::setAllowForSessionCookies(const QStringList &list)
{
    m_exceptions_allowForSession = list;
    std::sort(m_exceptions_allowForSession.begin(), m_exceptions_allowForSession.end());
    updatePolicySnapshot();
    applyRules();
    m_saveTimer->changeOccurred();
}

void CookieJar::applyRules()
{
    if (!m_store)
        return;
    foreach (const QNetworkCookie &cookie, m_cookies) {
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

bool CookieJar::filterTrackingCookies() const
{
    return this->m_filterTrackingCookies;
}

void CookieJar::setFilterTrackingCookies(bool filterTrackingCookies)
{
    this->m_filterTrackingCookies = filterTrackingCookies;
    updatePolicySnapshot();
}
