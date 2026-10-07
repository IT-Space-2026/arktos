/**
 * Copyright (c) 2009, Benjamin C. Meyer <ben@meyerhome.net>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Benjamin Meyer nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "adblockmanager.h"

#include "autosaver.h"
#include "adblockdialog.h"
#include "adblocknetwork.h"
#include "adblockpage.h"
#include "adblockrequestinterceptor.h"
#include "adblockresourcehandler.h"
#include "adblockschemeaccesshandler.h"
#include "adblocksubscription.h"
#include "browserpaths.h"
#include "networkaccessmanager.h"

#include <qdatetime.h>
#include <qhash.h>
#include <qmessagebox.h>
#include <qregularexpression.h>
#include <qstringlist.h>
#include <qsettings.h>
#include <qurlquery.h>
#include <qwebengineprofile.h>

#include <qdebug.h>

// #define ADBLOCKMANAGER_DEBUG

AdBlockManager *AdBlockManager::s_adBlockManager = nullptr;

AdBlockManager::AdBlockManager(QObject *parent)
    : QObject(parent)
    , m_loaded(false)
    , m_enabled(true)
    , m_saveTimer(new AutoSaver(this))
    , m_adBlockDialog(nullptr)
    , m_adBlockNetwork(nullptr)
    , m_adBlockPage(nullptr)
{
    connect(this, &AdBlockManager::rulesChanged,
            m_saveTimer, &AutoSaver::changeOccurred);
}

AdBlockManager::~AdBlockManager()
{
    m_saveTimer->saveIfNeccessary();
}

AdBlockManager *AdBlockManager::instance()
{
    if (!s_adBlockManager) {
        // Set a parent that will delete us before the application exits
        s_adBlockManager = new AdBlockManager(NetworkAccessManager::instance());
    }
    return s_adBlockManager;
}

bool AdBlockManager::isEnabled() const
{
    if (!m_loaded) {
        AdBlockManager *that = const_cast<AdBlockManager*>(this);
        that->load();
    }
    return m_enabled;
}

void AdBlockManager::setEnabled(bool enabled)
{
    if (isEnabled() == enabled)
        return;
    m_enabled = enabled;
    if (enabled) {
        // TELEM01: switching the blocker on is the user's "yes" to
        // remote filter lists — gate defers the fetch until here.
        grantRemoteLists();
    }
    emit rulesChanged();
}

AdBlockManager::RemoteListsConsent AdBlockManager::remoteListsConsent()
{
    const int value = QSettings().value(
        QLatin1String("AdBlock/remoteListsConsent"),
        int(RemoteListsUndecided)).toInt();
    if (value < int(RemoteListsUndecided) || value > int(RemoteListsGranted))
        return RemoteListsUndecided;
    return RemoteListsConsent(value);
}

void AdBlockManager::setRemoteListsConsent(RemoteListsConsent consent)
{
    QSettings().setValue(QLatin1String("AdBlock/remoteListsConsent"),
                         int(consent));
}

bool AdBlockManager::remoteListsAllowed()
{
    return remoteListsConsent() == RemoteListsGranted;
}

void AdBlockManager::grantRemoteLists()
{
    if (remoteListsConsent() != RemoteListsGranted)
        setRemoteListsConsent(RemoteListsGranted);
    // Only stale or never-fetched remote lists need the kick — a
    // freshly downloaded list should not be re-fetched on every
    // enable.
    const QDateTime now = QDateTime::currentDateTime();
    const QList<AdBlockSubscription*> list = subscriptions();
    for (AdBlockSubscription *subscription : list) {
        if (subscription->location().scheme() == QLatin1String("file"))
            continue;
        if (subscription->lastUpdate().isValid()
            && subscription->lastUpdate().addDays(7) >= now)
            continue;
        subscription->updateNow();
    }
}

void AdBlockManager::maybePromptForListConsent(QWidget *parent)
{
    if (remoteListsConsent() != RemoteListsUndecided || !isEnabled())
        return;
    bool hasRemoteSubscription = false;
    const QList<AdBlockSubscription*> list = subscriptions();
    for (const AdBlockSubscription *subscription : list) {
        const QString scheme = subscription->location().scheme();
        if (scheme == QLatin1String("http")
            || scheme == QLatin1String("https"))
            hasRemoteSubscription = true;
    }
    if (!hasRemoteSubscription)
        return;
    const QMessageBox::StandardButton choice = QMessageBox::question(parent,
        tr("Download Ad-Blocking Filter Lists?"),
        tr("Arora is configured to block ads and trackers using the "
           "community filter lists EasyList, EasyPrivacy, and uBlock "
           "filters. Enabling them downloads the lists once from "
           "easylist.to and ublockorigin.github.io and refreshes them "
           "about once a week.\n\nUntil you choose, no list downloads "
           "are made and the blocker simply has no remote rules. You "
           "can change this later under Tools > Ad Block."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (choice == QMessageBox::Yes)
        grantRemoteLists();
    else
        setRemoteListsConsent(RemoteListsDeclined);
}

AdBlockNetwork *AdBlockManager::network()
{
    if (!m_adBlockNetwork) {
        m_adBlockNetwork = new AdBlockNetwork(this);
        connect(this, &AdBlockManager::rulesChanged,
                m_adBlockNetwork, &AdBlockNetwork::rebuildRules);
        m_adBlockNetwork->rebuildRules();
    }
    return m_adBlockNetwork;
}

void AdBlockManager::installOnProfile(QWebEngineProfile *profile,
                                      bool deferInitialRules)
{
    if (deferInitialRules && !m_adBlockNetwork) {
        // PERF03: the interceptor needs a live matcher object to hold,
        // but building its first snapshot parses every subscribed list
        // — on the startup path that work is deferred to a queued
        // slot after the first window is shown (postLaunch).  Until
        // then the interceptor sees an empty ruleset (allow-all).
        m_adBlockNetwork = new AdBlockNetwork(this);
        connect(this, &AdBlockManager::rulesChanged,
                m_adBlockNetwork, &AdBlockNetwork::rebuildRules);
    } else {
        // Force rules to load and the matcher snapshot to be built on
        // the GUI thread before the interceptor starts seeing
        // requests on the WebEngine IO thread.
        network()->rebuildRules();
    }
    // PRIV01: a profile accepts exactly one interceptor — callers that
    // need the privacy composite (prepareProfile) replace this right
    // after; standalone profiles (autotests) keep the adblock-only one.
    profile->setUrlRequestInterceptor(new AdBlockRequestInterceptor(network(), this));
    // abp:subscribe?... links are handled by a real url-scheme handler;
    // the scheme itself is registered in main() before QApplication.
    profile->installUrlSchemeHandler(AdBlockSchemeAccessHandler::schemeName(),
                                     new AdBlockSchemeAccessHandler(this));
    // arora-resource:// serves the bundled $redirect= stub resources.
    profile->installUrlSchemeHandler(AdBlockResourceHandler::schemeName(),
                                     new AdBlockResourceHandler(this));
}

AdBlockPage *AdBlockManager::page()
{
    if (!m_adBlockPage)
        m_adBlockPage = new AdBlockPage(this);
    return m_adBlockPage;
}

static QUrl customSubscriptionLocation()
{
    QString fileName = BrowserPaths::dataFilePath(QLatin1String("adblock_subscription_custom"));
    return QUrl::fromLocalFile(fileName);
}

QUrl AdBlockManager::customSubscriptionUrl()
{
    QUrl location = customSubscriptionLocation();
    QString encodedUrl = QString::fromUtf8(location.toEncoded());
    QUrl url(QString(QLatin1String("abp:subscribe?location=%1&title=%2"))
            .arg(encodedUrl)
            .arg(tr("Custom Rules")));
    return url;
}

AdBlockSubscription *AdBlockManager::customRules()
{
    QUrl location = customSubscriptionLocation();
    for (AdBlockSubscription *subscription : m_subscriptions) {
        if (subscription->location() == location)
            return subscription;
    }
    QUrl url = customSubscriptionUrl();
    AdBlockSubscription *customAdBlockSubscription = new AdBlockSubscription(url, this);
    addSubscription(customAdBlockSubscription);
    return customAdBlockSubscription;
}

QList<AdBlockSubscription*> AdBlockManager::subscriptions() const
{
    if (!m_loaded) {
        AdBlockManager *that = const_cast<AdBlockManager*>(this);
        that->load();
    }
    return m_subscriptions;
}

void AdBlockManager::removeSubscription(AdBlockSubscription *subscription)
{
    if (!subscription)
        return;
#if defined(ADBLOCKMANAGER_DEBUG)
    qDebug() << "AdBlockManager::" << __FUNCTION__ << subscription->location();
#endif
    m_saveTimer->saveIfNeccessary();
    m_subscriptions.removeOne(subscription);
    if (subscription->parent() == this)
        subscription->deleteLater();
    emit rulesChanged();
}

void AdBlockManager::addSubscription(AdBlockSubscription *subscription)
{
    if (!subscription)
        return;
#if defined(ADBLOCKMANAGER_DEBUG)
    qDebug() << "AdBlockManager::" << __FUNCTION__ << subscription->location();
#endif
    m_subscriptions.append(subscription);
    connect(subscription, &AdBlockSubscription::rulesChanged, this, &AdBlockManager::rulesChanged);
    connect(subscription, &AdBlockSubscription::changed, this, &AdBlockManager::rulesChanged);
    emit rulesChanged();
}

void AdBlockManager::save()
{
#if defined(ADBLOCKMANAGER_DEBUG)
    qDebug() << "AdBlockManager::" << __FUNCTION__ << m_loaded;
#endif
    if (!m_loaded)
        return;

    QSettings settings;
    settings.beginGroup(QLatin1String("AdBlock"));
    settings.setValue(QLatin1String("enabled"), m_enabled);
    QStringList subscriptions;
    for (AdBlockSubscription *subscription : m_subscriptions) {
        if (!subscription)
            continue;
        subscriptions.append(QString::fromUtf8(subscription->url().toEncoded()));
        subscription->saveRules();
    }
    settings.setValue(QLatin1String("subscriptions"), subscriptions);
}

void AdBlockManager::load()
{
#if defined(ADBLOCKMANAGER_DEBUG)
    qDebug() << "AdBlockManager::" << __FUNCTION__ << m_loaded;
#endif

    if (m_loaded)
        return;
    m_loaded = true;

    QSettings settings;
    settings.beginGroup(QLatin1String("AdBlock"));
    m_enabled = settings.value(QLatin1String("enabled"), m_enabled).toBool();

    // Default subscriptions point at live mirrors of the community
    // lists; the historical adblockplus.mozdev.org host is dead.
    QStringList defaultSubscriptions;
    defaultSubscriptions.append(QString::fromUtf8(customSubscriptionUrl().toEncoded()));
    defaultSubscriptions.append(QLatin1String("abp:subscribe?location=https%3A%2F%2Feasylist.to%2Feasylist%2Feasylist.txt&title=EasyList"));
    defaultSubscriptions.append(QLatin1String("abp:subscribe?location=https%3A%2F%2Feasylist.to%2Feasylist%2Feasyprivacy.txt&title=EasyPrivacy"));
    defaultSubscriptions.append(QLatin1String("abp:subscribe?location=https%3A%2F%2Fublockorigin.github.io%2FuAssets%2Ffilters%2Ffilters.txt&title=uBlock%20filters"));

    QStringList subscriptions = settings.value(QLatin1String("subscriptions"), defaultSubscriptions).toStringList();

    // Upgrade stored subscriptions pointing at dead list hosts; the
    // file name is preserved so an easyprivacy subscription keeps
    // tracking EasyPrivacy.
    static const QStringList deadListHosts = {
        QStringLiteral("adblockplus.mozdev.org"),
        QStringLiteral("easylist.adblockplus.org"),
        QStringLiteral("easylist-downloads.adblockplus.org"),
    };
    for (QString &subscription : subscriptions) {
        const QUrl url = QUrl::fromEncoded(subscription.toUtf8());
        if (url.scheme() != QLatin1String("abp"))
            continue;
        const QUrlQuery query(url);
        const QUrl location = QUrl(query.queryItemValue(
            QLatin1String("location"), QUrl::FullyDecoded));
        if (!deadListHosts.contains(location.host()))
            continue;
        QString fileName = location.fileName();
        if (!fileName.endsWith(QLatin1String(".txt")))
            fileName = QLatin1String("easylist.txt");
        const QString replacement =
            QLatin1String("https://easylist.to/easylist/") + fileName;
        QUrlQuery updated;
        updated.addQueryItem(QLatin1String("location"), replacement);
        updated.addQueryItem(QLatin1String("title"),
                             query.queryItemValue(QLatin1String("title"),
                                                  QUrl::PrettyDecoded));
        if (!query.queryItemValue(QLatin1String("enabled"),
                                QUrl::PrettyDecoded).isEmpty())
            updated.addQueryItem(QLatin1String("enabled"),
                                 query.queryItemValue(
                                     QLatin1String("enabled"),
                                     QUrl::PrettyDecoded));
        QUrl migrated;
        migrated.setScheme(QLatin1String("abp"));
        migrated.setPath(QLatin1String("subscribe"));
        migrated.setQuery(updated);
        subscription = QString::fromUtf8(migrated.toEncoded());
    }

    for (const QString &subscription : subscriptions) {
        QUrl url = QUrl::fromEncoded(subscription.toUtf8());
        AdBlockSubscription *adBlockSubscription = new AdBlockSubscription(url, this);
        // Skip a stored entry that duplicates an already-loaded
        // subscription: the custom-rules subscription can be created
        // on demand before load() runs.
        for (const AdBlockSubscription *existing : m_subscriptions) {
            if (existing->location() == adBlockSubscription->location()) {
                delete adBlockSubscription;
                adBlockSubscription = nullptr;
                break;
            }
        }
        if (!adBlockSubscription)
            continue;
        connect(adBlockSubscription, &AdBlockSubscription::rulesChanged, this, &AdBlockManager::rulesChanged);
        connect(adBlockSubscription, &AdBlockSubscription::changed, this, &AdBlockManager::rulesChanged);
        m_subscriptions.append(adBlockSubscription);
    }
}

QString AdBlockManager::siteWhitelistFilter(const QString &host)
{
    // IDN hosts ride the filter in punycode — that is the spelling the
    // matcher sees in the encoded request url.
    const QByteArray ace = QUrl::toAce(host);
    const QString encoded = ace.isEmpty()
        ? host.toLower() : QString::fromLatin1(ace).toLower();
    // Anything outside a strict hostname shape (slashes, whitespace,
    // separators, IPv6 colons) would corrupt the ABP anchor syntax.
    static const QRegularExpression validHost(
        QStringLiteral("^[a-z0-9]([a-z0-9.-]*[a-z0-9])?$"));
    if (!validHost.match(encoded).hasMatch())
        return QString();
    return QLatin1String("@@||") + encoded + QLatin1String("^$document");
}

bool AdBlockManager::isSiteWhitelisted(const QString &host)
{
    const QString filter = siteWhitelistFilter(host);
    if (filter.isEmpty())
        return false;
    const QList<AdBlockRule> rules = customRules()->allRules();
    for (const AdBlockRule &rule : rules) {
        if (rule.isEnabled() && rule.filter() == filter)
            return true;
    }
    return false;
}

void AdBlockManager::setSiteWhitelisted(const QString &host, bool whitelisted)
{
    const QString filter = siteWhitelistFilter(host);
    if (filter.isEmpty())
        return;
    AdBlockSubscription *custom = customRules();
    const QList<AdBlockRule> rules = custom->allRules();
    bool found = false;
    // Walk backwards so removeRule keeps the earlier offsets valid.
    for (int i = rules.count() - 1; i >= 0; --i) {
        if (rules.at(i).filter() != filter)
            continue;
        found = true;
        if (!whitelisted)
            custom->removeRule(i);
    }
    if (whitelisted && !found)
        custom->addRule(AdBlockRule(filter));
}

AdBlockDialog *AdBlockManager::showDialog()
{
    if (!m_adBlockDialog) {
        m_adBlockDialog = new AdBlockDialog(nullptr);
        m_adBlockDialog->setAttribute(Qt::WA_DeleteOnClose, true);
    }
    m_adBlockDialog->show();
    return m_adBlockDialog;
}

