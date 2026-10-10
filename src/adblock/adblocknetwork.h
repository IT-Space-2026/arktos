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

#ifndef ADBLOCKNETWORK_H
#define ADBLOCKNETWORK_H

#include "adblockrule.h"

#include <memory>

#include <qhash.h>
#include <qobject.h>
#include <qreadwritelock.h>
#include <qurl.h>

class AdBlockRustEngine;
class QJsonObject;

// What the matcher decided for one request.
struct AdBlockDecision {
    enum Action { Allow, Block, Redirect } action;
    // Set on Redirect: canonical name of a bundled stub resource
    // (served on the arora-resource:// scheme).
    QString redirectResource;
    // Set on Redirect (adblock-rust engine only): a literal URL to
    // load instead — a data: URL carrying the stub payload or the
    // $removeparam-rewritten request URL.  Takes precedence over
    // redirectResource.
    QString redirectUrl;
    // Query-parameter specs to strip (uBO $removeparam): entries are
    // "*", a parameter name, or a /regular expression/.
    QStringList removeParams;

    AdBlockDecision() : action(Allow) { }
};

/*
    Matches request URLs against the adblock network rules.

    Under Qt WebKit this class hooked into the application's
    QNetworkAccessManager and returned a blocked QNetworkReply.  Qt
    WebEngine does its own networking in the Chromium IO thread, so all
    request blocking now goes through the profile's
    QWebEngineUrlRequestInterceptor (AdBlockRequestInterceptor), which
    calls match() from that thread.

    Because the interceptor runs off the GUI thread, match() never
    touches the live AdBlockSubscription objects: rebuildRules() (GUI
    thread only) copies the enabled network rules into a snapshot that
    match() walks under a read lock.
*/
class AdBlockNetwork : public QObject
{
    Q_OBJECT

public:
    AdBlockNetwork(QObject *parent = nullptr);
#if defined(ARORA_ADBLOCK_RUST)
    ~AdBlockNetwork();
#endif

    // Thread-safe; called by the request interceptor on the IO thread.
    // firstPartyUrl may be empty (treated as the request's own party);
    // resourceType is a QWebEngineUrlRequestInfo::ResourceType value.
    AdBlockDecision match(const QUrl &requestUrl,
                          const QUrl &firstPartyUrl = QUrl(),
                          int resourceType = -1) const;
    // The unindexed linear scan kept as the reference implementation:
    // differential tests assert match() produces identical decisions.
    // Do not call from the interceptor.
    AdBlockDecision matchLinear(const QUrl &requestUrl,
                                const QUrl &firstPartyUrl = QUrl(),
                                int resourceType = -1) const;
    bool shouldBlock(const QUrl &url) const;

    // ADB06: true while the snapshot delegates matching to the Rust
    // engine — only in CONFIG+=adblock_rust builds with the engine
    // selected and enabled.  Always false in native-only builds.
    bool rustEngineActive() const;

#if defined(ARORA_ADBLOCK_RUST)
    // The native C++ matcher, kept compiled in under the Rust flag so
    // the --adblock-rust-smoke comparison harness can diff the two
    // engines on identical input.  Do not call from the interceptor.
    AdBlockDecision matchNative(const QUrl &requestUrl,
                                const QUrl &firstPartyUrl = QUrl(),
                                int resourceType = -1) const;
    // Cosmetic payload for a document URL from the Rust engine
    // ({"hide":[],"generichide":bool,"script":""}); empty when the
    // engine is not loaded.
    QJsonObject rustCosmetic(const QUrl &documentUrl) const;
#endif

public slots:
    // Snapshots the current subscriptions' network rules.  GUI thread only.
    void rebuildRules();

private:
    // Requires m_lock held (read) and m_enabled already checked.
#if defined(ARORA_ADBLOCK_RUST)
    // Document-level unbreak scan shared by the composite match():
    // 1 = $document page exception (allow everything), 2 =
    // $genericblock page (native matcher handles it), 0 = neither.
    int documentUnbreakUnlocked(const QString &documentString,
                                const QString &documentHost) const;
#endif
    AdBlockDecision matchNativeUnlocked(
            const QUrl &requestUrl,
            const QUrl &firstPartyUrl,
            int resourceType) const;
    AdBlockDecision matchLinearUnlocked(
            const QUrl &requestUrl,
            const QUrl &firstPartyUrl,
            int resourceType) const;

    // Candidate prefilter over one flat rule list.  Every rule whose
    // pattern can match a URL contains its matchToken() verbatim, so
    // indexing the tokens by their leading 4 characters lets match()
    // check only the rules that can possibly match plus the generic
    // (untokenizable) ones — same candidates a linear scan would try,
    // same order, without walking the whole list per request.
    struct RuleIndex {
        QList<int> generic;   // rules with no usable literal token
        // Exception lists only: page-level modifiers ($document,
        // $elemhide, $generichide, $genericblock) matched against the
        // document URL once per request, not per rule list.
        QList<int> pageRules;
        QStringList tokens;               // unique tokens, lowercased
        QVector<QVector<int>> tokenRules; // token -> rule indices
        // First 4 chars of each token packed as a key -> token ids.
        QHash<quint64, QList<int>> grams;
    };

    struct SubscriptionRules {
        QList<AdBlockRule> exceptionRules;
        QList<AdBlockRule> blockRules;
        QList<AdBlockRule> removeParamRules;
        QList<AdBlockRule> removeParamExceptions;
        RuleIndex exceptionIndex;
        RuleIndex blockIndex;
        RuleIndex removeParamIndex;
        RuleIndex removeParamExceptionIndex;
    };

    static void buildIndex(const QList<AdBlockRule> &rules,
                           bool splitPageRules, RuleIndex *index);
    static QList<int> indexCandidates(const RuleIndex &index,
                                      const QString &loweredUrl);

    QList<SubscriptionRules> m_subscriptions;
    bool m_enabled;
    mutable QReadWriteLock m_lock;

#if defined(ARORA_ADBLOCK_RUST)
    std::unique_ptr<AdBlockRustEngine> m_rustEngine; // swapped under m_lock
#endif
};

#endif // ADBLOCKNETWORK_H
