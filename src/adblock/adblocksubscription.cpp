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

#include "adblocksubscription.h"

#include "adblockmanager.h"
#include "browserpaths.h"
#include "networkaccessmanager.h"

#include <qcryptographichash.h>
#include <qdebug.h>
#include <qfile.h>
#include <qnetworkreply.h>
#include <qset.h>
#include <qtextstream.h>
#include <qurlquery.h>

// #define ADBLOCKSUBSCRIPTION_DEBUG

AdBlockSubscription::AdBlockSubscription(const QUrl &url, QObject *parent)
    : QObject(parent)
    , m_url(url.toEncoded())
    , m_enabled(false)
    , m_downloading(nullptr)
{
    // A download finishing while the application (and the
    // NetworkAccessManager singleton) is being torn down is a crash —
    // subscriptions are leaked singleton children, so hook aboutToQuit
    // rather than relying on a destructor that never runs.
    if (QCoreApplication *app = QCoreApplication::instance()) {
        connect(app, &QCoreApplication::aboutToQuit, this, [this]() {
            if (!m_downloading)
                return;
            disconnect(m_downloading, nullptr, this, nullptr);
            m_downloading->abort();
            m_downloading->deleteLater();
            m_downloading = nullptr;
        });
    }
    parseUrl(url);
}

AdBlockSubscription::~AdBlockSubscription()
{
    if (m_downloading) {
        disconnect(m_downloading, nullptr, this, nullptr);
        m_downloading->abort();
        m_downloading->deleteLater();
    }
}

void AdBlockSubscription::parseUrl(const QUrl &url)
{
#if defined(ADBLOCKSUBSCRIPTION_DEBUG)
    qDebug() << "AdBlockSubscription::" << __FUNCTION__ << url;
#endif
    if (url.scheme() != QLatin1String("abp"))
        return;
    if (url.path() != QLatin1String("subscribe"))
        return;

    // QUrl::PrettyDecoded keeps '+' literal like QUrl::fromPercentEncoding did
    const QUrlQuery query(url);
    m_title = query.queryItemValue(QLatin1String("title"), QUrl::PrettyDecoded);
    m_enabled = query.queryItemValue(QLatin1String("enabled"), QUrl::PrettyDecoded) != QLatin1String("false");
    m_location = query.queryItemValue(QLatin1String("location"), QUrl::FullyDecoded).toUtf8();
    const QString lastUpdateString = query.queryItemValue(QLatin1String("lastUpdate"), QUrl::FullyDecoded);
    m_lastUpdate = QDateTime::fromString(lastUpdateString, Qt::ISODate);
    loadRules();
}

QUrl AdBlockSubscription::url() const
{
    QUrl url;
    url.setScheme(QLatin1String("abp"));
    url.setPath(QLatin1String("subscribe"));

    QUrlQuery query;
    query.addQueryItem(QLatin1String("location"), QString::fromUtf8(m_location));
    query.addQueryItem(QLatin1String("title"), m_title);
    if (!m_enabled)
        query.addQueryItem(QLatin1String("enabled"), QLatin1String("false"));
    if (m_lastUpdate.isValid())
        query.addQueryItem(QLatin1String("lastUpdate"), m_lastUpdate.toString(Qt::ISODate));
    url.setQuery(query);
    return url;
}

bool AdBlockSubscription::isEnabled() const
{
    return m_enabled;
}

void AdBlockSubscription::setEnabled(bool enabled)
{
    if (m_enabled == enabled)
        return;
    m_enabled = enabled;
    populateCache();
    emit changed();
}

QString AdBlockSubscription::title() const
{
    return m_title;
}

void AdBlockSubscription::setTitle(QString title)
{
    if (m_title == title)
        return;
    m_title = std::move(title);
    emit changed();
}

QUrl AdBlockSubscription::location() const
{
    return QUrl::fromEncoded(m_location);
}

void AdBlockSubscription::setLocation(const QUrl &url)
{
    if (url == location())
        return;
    m_location = url.toEncoded();
    m_lastUpdate = QDateTime();
    emit changed();
}

QDateTime AdBlockSubscription::lastUpdate() const
{
    return m_lastUpdate;
}

QString AdBlockSubscription::rulesFileName() const
{
    if (location().scheme() == QLatin1String("file"))
        return location().toLocalFile();

    if (m_location.isEmpty())
        return QString();

    QByteArray sha1 = QCryptographicHash::hash(m_location, QCryptographicHash::Sha1).toHex();
    QString fileName = BrowserPaths::dataFilePath(QString(QLatin1String("adblock_subscription_%1")).arg(QLatin1String(sha1)));
    return fileName;
}

// A subscription list is at most a few megabytes (EasyList ≈ 4 MB);
// anything larger is corrupt or hostile and is not parsed.  64 MB is a
// generous bound.
static const qint64 MaximumRulesFileSize = 64 * 1024 * 1024;
// Filter lines are a few hundred characters; cap reads so a giant
// single-line file cannot produce a giant QString per rule.
static const qint64 MaximumLineLength = 16 * 1024;
// Bound total rules so a hostile list cannot exhaust memory compiling
// regular expressions (EasyList is ~60k rules; 500k is generous).
static const int MaximumRuleCount = 500 * 1000;

void AdBlockSubscription::loadRules()
{
    QString fileName = rulesFileName();
#if defined(ADBLOCKSUBSCRIPTION_DEBUG)
    qDebug() << "AdBlockSubscription::" << __FUNCTION__ << fileName;
#endif
    QFile file(fileName);
    if (file.exists()) {
        if (file.size() > MaximumRulesFileSize) {
            qWarning() << "AdBlockSubscription::" << __FUNCTION__ << "adblock file too large" << fileName;
        } else if (!file.open(QFile::ReadOnly)) {
            qWarning() << "AdBlockSubscription::" << __FUNCTION__ << "Unable to open adblock file for reading" << fileName;
        } else {
            QTextStream textStream(&file);
            QString header = textStream.readLine(1024);
            if (!header.startsWith(QLatin1String("[Adblock"))) {
                qWarning() << "AdBlockSubscription::" << __FUNCTION__ << "adblock file does not start with [Adblock" << fileName << "Header:" << header;
                file.close();
                file.remove();
                m_lastUpdate = QDateTime();
            } else {
                m_rules.clear();
                while (!textStream.atEnd()
                       && m_rules.count() < MaximumRuleCount) {
                    QString line = textStream.readLine(MaximumLineLength);
                    m_rules.append(AdBlockRule(line));
                }
                populateCache();
                emit rulesChanged();
            }
        }
    }

    const bool stale = !m_lastUpdate.isValid()
        || m_lastUpdate.addDays(7) < QDateTime::currentDateTime();
    // TELEM01: this is the automatic refresh path — a remote list may
    // only be fetched once the user consented to list downloads (the
    // first-launch prompt, an abp: subscribe, Update Subscription, or
    // enabling the blocker).  file: subscriptions "update" by
    // re-reading a local file, so no consent applies to them; and an
    // explicit updateNow() call stays an on-demand fetch either way.
    if (stale && (location().scheme() == QLatin1String("file")
                  || AdBlockManager::remoteListsAllowed()))
        updateNow();
}

void AdBlockSubscription::updateNow()
{
#if defined(ADBLOCKSUBSCRIPTION_DEBUG)
    qDebug() << "AdBlockSubscription::" << __FUNCTION__ << location();
#endif
    if (m_downloading) {
#if defined(ADBLOCKSUBSCRIPTION_DEBUG)
        qDebug() << "AdBlockSubscription::" << __FUNCTION__ << "already downloading, stopping";
#endif
        return;
    }

    if (!location().isValid()) {
#if defined(ADBLOCKSUBSCRIPTION_DEBUG)
        qDebug() << "AdBlockSubscription::" << __FUNCTION__ << location() << "isn't valid";
#endif
        return;
    }

    if (location().scheme() == QLatin1String("file")) {
#if defined(ADBLOCKSUBSCRIPTION_DEBUG)
        qDebug() << "AdBlockSubscription::" << __FUNCTION__ << "local file, not downloading";
#endif
        m_lastUpdate = QDateTime::currentDateTime();
        loadRules();
        emit changed();
        return;
    }

    QNetworkRequest request(location());
    QNetworkReply *reply = NetworkAccessManager::instance()->get(request);
    m_downloading = reply;
    connect(reply, &QNetworkReply::finished, this, &AdBlockSubscription::rulesDownloaded);
}

void AdBlockSubscription::rulesDownloaded()
{
#if defined(ADBLOCKSUBSCRIPTION_DEBUG)
    qDebug() << "AdBlockSubscription::" << __FUNCTION__ << rulesFileName();
#endif
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) {
#if defined(ADBLOCKSUBSCRIPTION_DEBUG)
        qDebug() << "AdBlockSubscription::" << __FUNCTION__ << "no reply?";
#endif
        return;
    }

    // Bound the response before buffering it into the rules file.
    const QByteArray response = reply->size() <= MaximumRulesFileSize
        ? reply->readAll() : QByteArray();
    QUrl redirect = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
    reply->close();
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        qWarning() << "AdBlockSubscription::" << __FUNCTION__ << "error" << reply->errorString();
        return;
    }

    if (redirect.isValid()) {
#if defined(ADBLOCKSUBSCRIPTION_DEBUG)
        qDebug() << "AdBlockSubscription::" << __FUNCTION__ << "redirect to:" << redirect;
#endif
        QNetworkRequest request(redirect);
        m_downloading = NetworkAccessManager::instance()->get(request);
        connect(m_downloading, &QNetworkReply::finished, this, &AdBlockSubscription::rulesDownloaded);
        return;
    }

    if (response.isEmpty()) {
        qWarning() << "AdBlockSubscription::" << __FUNCTION__ << "empty response";
        return;
    }

    QString fileName = rulesFileName();
    QFile file(fileName);
    if (!file.open(QFile::WriteOnly)) {
        qWarning() << "AdBlockSubscription::" << __FUNCTION__ << "Unable to open adblock file for writing:" << fileName;
        return;
    }
    if (file.write(response) != response.size()) {
        qWarning() << "AdBlockSubscription::" << __FUNCTION__ << "Unable to write adblock file:" << fileName;
        return;
    }
    // Close before reloading: QFile's internal buffer flushes on
    // close, and loadRules would otherwise reopen an empty file and
    // clear m_lastUpdate as "not an adblock list".
    file.close();
    m_lastUpdate = QDateTime::currentDateTime();
    loadRules();
    emit changed();
    m_downloading = nullptr;
}

void AdBlockSubscription::saveRules()
{
#if defined(ADBLOCKSUBSCRIPTION_DEBUG)
    qDebug() << "AdBlockSubscription::" << __FUNCTION__ << rulesFileName() << m_rules.count();
#endif
    QString fileName = rulesFileName();
    if (fileName.isEmpty())
        return;

    QFile file(fileName);
    if (!file.open(QFile::ReadWrite | QIODevice::Truncate)) {
        qWarning() << "AdBlockSubscription::" << __FUNCTION__ << "Unable to open adblock file for writing:" << fileName;
        return;
    }

    QTextStream textStream(&file);
    textStream << "[Adblock Plus 0.7.1]" << Qt::endl;
    for (const AdBlockRule &rule : m_rules)
        textStream << rule.filter() << Qt::endl;
}

QList<const AdBlockRule*> AdBlockSubscription::pageRules() const
{
    return m_pageRules;
}

QList<const AdBlockRule*> AdBlockSubscription::networkExceptionRules() const
{
    return m_networkExceptionRules;
}

QList<const AdBlockRule*> AdBlockSubscription::networkBlockRules() const
{
    return m_networkBlockRules;
}

const AdBlockRule *AdBlockSubscription::allow(const QString &urlString) const
{
    for (const AdBlockRule *rule : m_networkExceptionRules) {
        if (rule->networkMatch(urlString))
            return rule;
    }
    return nullptr;
}

const AdBlockRule *AdBlockSubscription::block(const QString &urlString) const
{
    for (const AdBlockRule *rule : m_networkBlockRules) {
        if (rule->networkMatch(urlString))
            return rule;
    }
    return nullptr;
}

QList<AdBlockRule> AdBlockSubscription::allRules() const
{
    return m_rules;
}

void AdBlockSubscription::addRule(const AdBlockRule &rule)
{
#if defined(ADBLOCKSUBSCRIPTION_DEBUG)
    qDebug() << "AdBlockSubscription::" << __FUNCTION__ << rule.filter();
#endif
    m_rules.append(rule);
    populateCache();
    emit rulesChanged();
}

void AdBlockSubscription::removeRule(int offset)
{
#if defined(ADBLOCKSUBSCRIPTION_DEBUG)
    qDebug() << "AdBlockSubscription::" << __FUNCTION__ << offset << m_rules.count();
#endif
    if (offset < 0 || offset >= m_rules.count())
        return;
    m_rules.removeAt(offset);
    populateCache();
    emit rulesChanged();
}

void AdBlockSubscription::replaceRule(const AdBlockRule &rule, int offset)
{
    if (offset < 0 || offset >= m_rules.count())
        return;
    m_rules[offset] = rule;
    populateCache();
    emit rulesChanged();
}

void AdBlockSubscription::populateCache()
{
    m_networkExceptionRules.clear();
    m_networkBlockRules.clear();
    m_pageRules.clear();
    if (!isEnabled())
        return;

    // $badfilter rules disable the identical filter (same text minus
    // the badfilter option) within this subscription.
    QSet<QString> disabled;
    for (const AdBlockRule &rule : m_rules) {
        if (rule.isBadFilter())
            disabled.insert(rule.badFilterKey());
    }

    for (int i = 0; i < m_rules.count(); ++i) {
        const AdBlockRule *rule = &m_rules.at(i);
        if (!rule->isEnabled() || !rule->isSupported()
            || rule->isBadFilter()
            || disabled.contains(rule->badFilterKey()))
            continue;

        if (rule->isCSSRule()) {
            m_pageRules.append(rule);
            continue;
        }

        if (rule->isException()) {
            m_networkExceptionRules.append(rule);
        } else {
            m_networkBlockRules.append(rule);
        }
    }
}

