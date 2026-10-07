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
#include "startupprofile.h"

#if defined(ARORA_ADBLOCK_RUST)
#include "adblockrustengine.h"

#include <qjsonobject.h>
#endif

#include <qdebug.h>
#include <qset.h>
#include <qurl.h>

#include <algorithm>

// #define ADBLOCKNETWORK_DEBUG

AdBlockNetwork::AdBlockNetwork(QObject *parent)
    : QObject(parent)
    , m_enabled(true)
{
}

#if defined(ARORA_ADBLOCK_RUST)
AdBlockNetwork::~AdBlockNetwork() = default;
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

AdBlockDecision AdBlockNetwork::matchLinear(const QUrl &url,
                                            const QUrl &firstPartyUrl,
                                            int resourceType) const
{
    AdBlockDecision decision;
    if (url.scheme() == QLatin1String("data"))
        return decision;

    QReadLocker locker(&m_lock);
    if (!m_enabled)
        return decision;
    return matchLinearUnlocked(url, firstPartyUrl, resourceType);
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

// The minimum literal-token length that is worth indexing.  Shorter
// tokens would hash to n-grams every URL contains, so those rules sit
// in the generic bucket with the untokenizable ones.
static const int MinimumTokenLength = 4;

// Packs the four UTF-16 code units at offset into a hash key.
static quint64 gramKey(const QString &text, int offset)
{
    return (quint64(text.at(offset).unicode()) << 48)
        | (quint64(text.at(offset + 1).unicode()) << 32)
        | (quint64(text.at(offset + 2).unicode()) << 16)
        | quint64(text.at(offset + 3).unicode());
}

void AdBlockNetwork::buildIndex(const QList<AdBlockRule> &rules,
                                bool splitPageRules, RuleIndex *index)
{
    *index = RuleIndex();
    QHash<QString, int> tokenIds;
    for (int i = 0; i < rules.count(); ++i) {
        const AdBlockRule &rule = rules.at(i);
        if (splitPageRules
            && (rule.isDocumentException() || rule.isElemHide()
                || rule.isGenericHide() || rule.isGenericBlock())) {
            index->pageRules.append(i);
            continue;
        }
        const QString token = rule.matchToken().toLower();
        if (token.size() < MinimumTokenLength) {
            index->generic.append(i);
            continue;
        }
        int id = tokenIds.value(token, -1);
        if (id == -1) {
            id = index->tokens.count();
            tokenIds.insert(token, id);
            index->tokens.append(token);
            index->tokenRules.append(QVector<int>());
            index->grams[gramKey(token, 0)].append(id);
        }
        index->tokenRules[id].append(i);
    }
}

// The rules that can possibly match the URL: the generic bucket plus
// every rule whose literal token occurs in it, in original list order
// so first-match-wins semantics are unchanged.
QList<int> AdBlockNetwork::indexCandidates(const RuleIndex &index,
                                           const QString &loweredUrl)
{
    QList<int> candidates = index.generic;
    if (loweredUrl.size() >= MinimumTokenLength) {
        QSet<int> checked;
        QList<int> hits;
        for (int i = 0;
             i + MinimumTokenLength <= loweredUrl.size(); ++i) {
            const auto it = index.grams.constFind(gramKey(loweredUrl, i));
            if (it == index.grams.constEnd())
                continue;
            for (const int id : it.value()) {
                if (checked.contains(id))
                    continue;
                checked.insert(id);
                if (loweredUrl.contains(index.tokens.at(id)))
                    hits += index.tokenRules.at(id).toList();
            }
        }
        candidates += hits;
        std::sort(candidates.begin(), candidates.end());
    }
    return candidates;
}

// Requires m_lock held (read) and m_enabled already checked.
AdBlockDecision AdBlockNetwork::matchNativeUnlocked(
        const QUrl &url, const QUrl &firstPartyUrl, int resourceType) const
{
    AdBlockDecision decision;

    const QString urlString = QString::fromUtf8(url.toEncoded());
    const QString urlLower = urlString.toLower();
    const QString documentHost =
            firstPartyUrl.isEmpty() ? QString() : firstPartyUrl.host();
    const QString documentString = firstPartyUrl.isEmpty()
            ? urlString : QString::fromUtf8(firstPartyUrl.toEncoded());

    // Document-level exceptions (uBO/ABP unbreak options): a $document
    // exception turns blocking off for the whole page, $genericblock
    // suppresses rules that are not domain-restricted.
    bool genericBlock = false;
    for (const SubscriptionRules &rules : m_subscriptions) {
        for (const int i : rules.exceptionIndex.pageRules) {
            const AdBlockRule &rule = rules.exceptionRules.at(i);
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
        const AdBlockRule *exception = nullptr;
        for (const int i : indexCandidates(rules.exceptionIndex,
                                           urlLower)) {
            const AdBlockRule &rule = rules.exceptionRules.at(i);
            if (rule.networkMatch(urlString, documentHost, resourceType)) {
                exception = &rule;
                break;
            }
        }
        for (const int i : indexCandidates(rules.blockIndex, urlLower)) {
            const AdBlockRule &rule = rules.blockRules.at(i);
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
        for (const int i : indexCandidates(rules.removeParamExceptionIndex,
                                           urlLower)) {
            const AdBlockRule &rule = rules.removeParamExceptions.at(i);
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
            for (const int i : indexCandidates(rules.removeParamIndex,
                                               urlLower)) {
                const AdBlockRule &rule = rules.removeParamRules.at(i);
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

// Requires m_lock held (read) and m_enabled already checked.
// The original unindexed scan; matchNativeUnlocked must return the
// same decision for every input, which the differential test checks.
AdBlockDecision AdBlockNetwork::matchLinearUnlocked(
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
        const AdBlockRule *exception = nullptr;
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
    StartupProfile::Scope profileScope("adblock rules snapshot");
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
        buildIndex(rules.exceptionRules, true, &rules.exceptionIndex);
        buildIndex(rules.blockRules, false, &rules.blockIndex);
        buildIndex(rules.removeParamRules, false, &rules.removeParamIndex);
        buildIndex(rules.removeParamExceptions, false,
                   &rules.removeParamExceptionIndex);
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
            enabled ? AdBlockRustEngine::create(rustText) : nullptr;
#endif

    QWriteLocker locker(&m_lock);
    m_subscriptions = snapshot;
    m_enabled = enabled;
#if defined(ARORA_ADBLOCK_RUST)
    m_rustEngine.reset(rustEngine);
#endif
}
