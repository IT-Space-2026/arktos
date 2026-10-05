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

#if defined(ARORA_ADBLOCK_RUST)
#include "adblockrustengine.h"

#include <qjsonobject.h>
#endif

#include <qdebug.h>
#include <qurl.h>

// #define ADBLOCKNETWORK_DEBUG

AdBlockNetwork::AdBlockNetwork(QObject *parent)
    : QObject(parent)
    , m_enabled(true)
#if defined(ARORA_ADBLOCK_RUST)
    , m_rustEngine(0)
#endif
{
}

#if defined(ARORA_ADBLOCK_RUST)
AdBlockNetwork::~AdBlockNetwork()
{
    delete m_rustEngine;
}
#endif

// ResourceTypeMainFrame from QWebEngineUrlRequestInfo::ResourceType.
static const int sc_mainFrameType = 0;

AdBlockDecision AdBlockNetwork::match(const QUrl &url,
                                      const QUrl &firstPartyUrl,
                                      int resourceType) const
{
    AdBlockDecision decision;
    if (url.scheme() == QLatin1String("data"))
        return decision;

    QReadLocker locker(&m_lock);
    if (!m_enabled)
        return decision;

#if defined(ARORA_ADBLOCK_RUST)
    if (m_rustEngine)
        return m_rustEngine->check(url, firstPartyUrl, resourceType);
#endif
    return matchNativeUnlocked(url, firstPartyUrl, resourceType);
}

#if defined(ARORA_ADBLOCK_RUST)
AdBlockDecision AdBlockNetwork::matchNative(const QUrl &url,
                                            const QUrl &firstPartyUrl,
                                            int resourceType) const
{
    AdBlockDecision decision;
    if (url.scheme() == QLatin1String("data"))
        return decision;

    QReadLocker locker(&m_lock);
    if (!m_enabled)
        return decision;
    return matchNativeUnlocked(url, firstPartyUrl, resourceType);
}

QJsonObject AdBlockNetwork::rustCosmetic(const QUrl &documentUrl) const
{
    QReadLocker locker(&m_lock);
    if (!m_enabled || !m_rustEngine)
        return QJsonObject();
    return m_rustEngine->cosmetic(documentUrl);
}
#endif

// Requires m_lock held (read) and m_enabled already checked.
AdBlockDecision AdBlockNetwork::matchNativeUnlocked(
        const QUrl &url, const QUrl &firstPartyUrl, int resourceType) const
{
    AdBlockDecision decision;

    const QString urlString = QString::fromUtf8(url.toEncoded());
    const QString documentHost =
            firstPartyUrl.isEmpty() ? QString() : firstPartyUrl.host();
    const QString documentString = firstPartyUrl.isEmpty()
            ? urlString : QString::fromUtf8(firstPartyUrl.toEncoded());

    // Document-level exceptions (uBO/ABP unbreak options): a $document
    // exception turns blocking off for the whole page, $genericblock
    // suppresses rules that are not domain-restricted.
    bool genericBlock = false;
    for (const SubscriptionRules &rules : m_subscriptions) {
        for (const AdBlockRule &rule : rules.exceptionRules) {
            if (rule.isDocumentException()
                && rule.networkMatch(documentString, documentHost,
                                     sc_mainFrameType))
                return decision;
            if (rule.isGenericBlock()
                && rule.networkMatch(documentString, documentHost,
                                     sc_mainFrameType))
                genericBlock = true;
        }
    }

    // Per-subscription order is preserved from the WebKit
    // implementation: the first subscription to produce a decision
    // wins, and inside a subscription an exception beats a block
    // (except that a block with $important overrides a non-important
    // exception, per uBO).
    for (const SubscriptionRules &rules : m_subscriptions) {
        const AdBlockRule *exception = 0;
        for (const AdBlockRule &rule : rules.exceptionRules) {
            if (rule.isElemHide() || rule.isGenericHide()
                || rule.isGenericBlock() || rule.isDocumentException())
                continue; // page-level modifiers, not request vetoes
            if (rule.networkMatch(urlString, documentHost, resourceType)) {
                exception = &rule;
                break;
            }
        }
        for (const AdBlockRule &rule : rules.blockRules) {
            if (genericBlock && !rule.hasDomainOption()
                && !rule.isException())
                continue; // generic rules suppressed for this page
            if (!rule.networkMatch(urlString, documentHost, resourceType))
                continue;
            if (exception
                && !(rule.isImportant() && !exception->isImportant())) {
                decision.action = AdBlockDecision::Allow;
                return decision;
            }
#if defined(ADBLOCKNETWORK_DEBUG)
            qDebug() << "AdBlockNetwork::" << __FUNCTION__
                     << "rule:" << rule.filter() << url;
#endif
            if (!rule.redirectResource().isEmpty()) {
                decision.action = AdBlockDecision::Redirect;
                decision.redirectResource = rule.redirectResource();
            } else {
                decision.action = AdBlockDecision::Block;
            }
            return decision;
        }
        if (exception)
            return decision; // allowed by this subscription
    }

    // $removeparam rules never block; they rewrite the query string.
    // An @@...$removeparam exception cancels stripping (bare = all
    // params, removeparam=name = just that name).
    bool exceptAll = false;
    QStringList excepted;
    for (const SubscriptionRules &rules : m_subscriptions) {
        for (const AdBlockRule &rule : rules.removeParamExceptions) {
            if (!rule.networkMatch(urlString, documentHost, resourceType))
                continue;
            const QString spec = rule.removeParam();
            if (spec.isEmpty() || spec == QLatin1String("*"))
                exceptAll = true;
            else if (!excepted.contains(spec))
                excepted.append(spec);
        }
    }
    if (!exceptAll) {
        QStringList specs;
        for (const SubscriptionRules &rules : m_subscriptions) {
            for (const AdBlockRule &rule : rules.removeParamRules) {
                if (!rule.networkMatch(urlString, documentHost, resourceType))
                    continue;
                const QString spec = rule.removeParam();
                if (spec.isEmpty() || excepted.contains(spec))
                    continue;
                if (spec == QLatin1String("*")) {
                    specs.clear();
                    specs.append(spec);
                    break;
                }
                if (!specs.contains(QLatin1String("*"))
                    && !specs.contains(spec))
                    specs.append(spec);
            }
            if (specs.contains(QLatin1String("*")))
                break;
        }
        decision.removeParams = specs;
    }
    return decision;
}

bool AdBlockNetwork::shouldBlock(const QUrl &url) const
{
    return match(url).action != AdBlockDecision::Allow;
}

void AdBlockNetwork::rebuildRules()
{
    AdBlockManager *manager = AdBlockManager::instance();
    const bool enabled = manager->isEnabled();
    const QList<AdBlockSubscription*> subscriptions = manager->subscriptions();

    QList<SubscriptionRules> snapshot;
    snapshot.reserve(subscriptions.count());
#if defined(ARORA_ADBLOCK_RUST)
    QByteArray rustText;
#endif
    for (const AdBlockSubscription *subscription : subscriptions) {
        if (!subscription->isEnabled())
            continue;
        SubscriptionRules rules;
        for (const AdBlockRule *rule : subscription->networkExceptionRules()) {
            if (!rule->removeParam().isEmpty())
                rules.removeParamExceptions.append(*rule);
            else
                rules.exceptionRules.append(*rule);
        }
        for (const AdBlockRule *rule : subscription->networkBlockRules()) {
            if (!rule->removeParam().isEmpty())
                rules.removeParamRules.append(*rule);
            else
                rules.blockRules.append(*rule);
        }
        snapshot.append(rules);
#if defined(ARORA_ADBLOCK_RUST)
        // The Rust engine parses the raw filter text itself; only
        // rules we can stand behind (enabled, no unimplementable
        // options) are fed to it.  $badfilter lines pass through —
        // adblock-rust resolves them natively.
        for (const AdBlockRule &rule : subscription->allRules()) {
            if (!rule.isEnabled() || !rule.isSupported())
                continue;
            rustText += rule.filter().toUtf8();
            rustText += '\n';
        }
#endif
    }

#if defined(ARORA_ADBLOCK_RUST)
    // Engine construction parses the whole corpus — do it before
    // taking the write lock so IO-thread readers are not stalled.
    AdBlockRustEngine *rustEngine =
            enabled ? AdBlockRustEngine::create(rustText) : 0;
#endif

    QWriteLocker locker(&m_lock);
    m_subscriptions = snapshot;
    m_enabled = enabled;
#if defined(ARORA_ADBLOCK_RUST)
    delete m_rustEngine;
    m_rustEngine = rustEngine;
#endif
}
