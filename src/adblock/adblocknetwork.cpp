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

#include "adblocknetwork.h"

#include "adblockmanager.h"
#include "adblocksubscription.h"

#include <qdebug.h>
#include <qurl.h>

// #define ADBLOCKNETWORK_DEBUG

AdBlockNetwork::AdBlockNetwork(QObject *parent)
    : QObject(parent)
    , m_enabled(true)
{
}

bool AdBlockNetwork::shouldBlock(const QUrl &url) const
{
    if (url.scheme() == QLatin1String("data"))
        return false;

    QReadLocker locker(&m_lock);
    if (!m_enabled)
        return false;

    const QString urlString = QString::fromUtf8(url.toEncoded());
    // Per-subscription order is preserved from the WebKit implementation:
    // the first subscription to produce a decision wins, and inside a
    // subscription an exception beats a block.
    for (const SubscriptionRules &rules : m_subscriptions) {
        for (const AdBlockRule &rule : rules.exceptionRules) {
            if (rule.networkMatch(urlString))
                return false;
        }
        for (const AdBlockRule &rule : rules.blockRules) {
            if (rule.networkMatch(urlString)) {
#if defined(ADBLOCKNETWORK_DEBUG)
                qDebug() << "AdBlockNetwork::" << __FUNCTION__ << "rule:" << rule.filter() << url;
#endif
                return true;
            }
        }
    }
    return false;
}

void AdBlockNetwork::rebuildRules()
{
    AdBlockManager *manager = AdBlockManager::instance();
    const bool enabled = manager->isEnabled();
    const QList<AdBlockSubscription*> subscriptions = manager->subscriptions();

    QList<SubscriptionRules> snapshot;
    snapshot.reserve(subscriptions.count());
    for (const AdBlockSubscription *subscription : subscriptions) {
        if (!subscription->isEnabled())
            continue;
        SubscriptionRules rules;
        const QList<AdBlockRule> allRules = subscription->allRules();
        for (const AdBlockRule &rule : allRules) {
            if (!rule.isEnabled() || rule.isCSSRule())
                continue;
            if (rule.isException())
                rules.exceptionRules.append(rule);
            else
                rules.blockRules.append(rule);
        }
        snapshot.append(rules);
    }

    QWriteLocker locker(&m_lock);
    m_subscriptions = snapshot;
    m_enabled = enabled;
}
