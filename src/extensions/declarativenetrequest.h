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

#ifndef DECLARATIVENETREQUEST_H
#define DECLARATIVENETREQUEST_H

#include <qatomic.h>
#include <qbytearray.h>
#include <qhash.h>
#include <qjsonarray.h>
#include <qjsonobject.h>
#include <qlist.h>
#include <qmutex.h>
#include <qobject.h>
#include <qreadwritelock.h>
#include <qregularexpression.h>
#include <qset.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qurl.h>
#include <qvector.h>

// MD34 — chrome.declarativeNetRequest subset.
//
// This class is the DECISION layer only: it owns the per-extension
// rule registry (session rules in memory, dynamic rules and static
// ruleset enable-overrides persisted under the extension's own
// extstorage/<id>/dnr.json like Chrome's store), validates and
// compiles rules, and publishes a copy-on-write snapshot the request
// interceptor evaluates on the WebEngine IO thread.  Applying the
// verdict — info.block(), info.redirect(), info.setHttpHeader() —
// stays Qt/engine-side in PrivacyRequestInterceptor.
//
// Served surface (invokeDeclarativeNetRequest drives these):
//   updateSessionRules / updateDynamicRules — atomic remove+add
//   getSessionRules / getDynamicRules       — normalized echo
//   getEnabledRulesets / updateEnabledRulesets
//   getAvailableStaticRuleCount             — guaranteed-pool count
//   isRegexSupported                        — honest RE2 check
//   testMatchOutcome                        — same evaluator
//   getMatchedRules + onRuleMatchedDebug    — feedback-granted log
//
// Typed verdicts the backend returns: no-permission (grant missing),
// unsupported (responseHeaders condition, response-side header ops,
// tabIds/excludedTabIds, setExtensionActionOptions), invalid-argument
// (malformed rule/options), duplicate-rule-id, rule-not-found,
// ruleset-not-found, quota-exceeded.
//
// Semantics deliberately narrowed (documented, not silent):
//   - precedence is the task-specified priority first, then
//     session > dynamic > static for same-priority same-action
//     candidates; within a source lower registration order wins.
//     Cross-extension ties resolve by action rank then most recently
//     registered extension (Chrome's install-recency analogue).
//   - allowAllRequests frame exemption keys on the frame's document
//     URL (firstPartyUrl) — no frame-id plumbing exists at this
//     layer, so the exemption is the host-normalized document url.
//   - domainType uses host equality / subdomain relation, not a PSL
//     eTLD+1 cut (no public-suffix table in this module).
//   - modifyHeaders applies REQUEST headers only; responseHeaders
//     conditions and response-side ops are typed unsupported — the
//     interceptor has no response hook.
//   - redirect 'url' targets are limited to http/https;
//     'extensionPath' resolves onto the extension's own
//     chrome-extension:// origin; regexSubstitution rides
//     QRegularExpression (the isRegexSupported honesty check keeps
//     RE2-incompatible patterns out).
//   - static ruleset resources load lazily from the bundle under
//     declarative_net_request.rule_resources; unreadable/invalid
//     entries skip like Chrome's ignored-rule behavior and record a
//     note rather than failing the extension.
//   - the 300k global static pool is unmodelled: each extension gets
//     its guaranteed 30k; getAvailableStaticRuleCount reports what
//     remains of that guarantee.
class DeclarativeNetRequest : public QObject
{
    Q_OBJECT

public:
    enum RuleSource {
        SourceSession = 0,
        SourceDynamic = 1,
        SourceStatic  = 2
    };

    enum Action {
        ActionNone = 0,
        ActionAllow,
        ActionAllowAllRequests,
        ActionBlock,
        ActionUpgradeScheme,
        ActionRedirect,
        ActionModifyHeaders
    };

    enum HeaderOp {
        HeaderAppend = 0,
        HeaderSet,
        HeaderRemove
    };

    struct RequestHeaderOp {
        QByteArray name;
        int op = HeaderSet;        // HeaderOp
        QByteArray value;
    };

    // Everything the IO-thread snapshot lane needs about a request —
    // the interceptor marshals it from QWebEngineUrlRequestInfo so
    // this module never touches engine types.
    struct EvalInput {
        QUrl url;
        QUrl firstPartyUrl;
        int resourceType = -1;     // QWebEngineUrlRequestInfo::ResourceType
        QByteArray method;
    };

    // The decision a request gets.  blocked wins over redirect; when
    // neither fired the header ops are applied in order by the caller.
    struct Verdict {
        bool blocked = false;
        QUrl redirectUrl;
        QVector<RequestHeaderOp> headerOps;
    };

    static DeclarativeNetRequest *instance();

    // IO-thread fast bail + the evaluation entry the interceptor
    // calls under m_reportToExtensions.  Lock-light: a relaxed read
    // on the rule count first, then a shared read on the snapshot.
    static int registeredRuleCount();
    static Verdict evaluate(const EvalInput &in);

    // chrome.declarativeNetRequest.isRegexSupported — honest verdict:
    // the pattern must compile under QRegularExpression AND stay
    // inside the RE2 feature envelope (no lookaround, backrefs,
    // atomic groups, possessive quantifiers, \Q..\E, \C, recursion).
    // reason answers 'syntaxError'/'unsupportedSyntax'-shaped text —
    // kept inside Chrome's IsRegexSupportedReason vocabulary via
    // 'syntaxError' plus a detail string for the note channel.
    static bool regexSupported(const QString &pattern,
                               bool caseSensitive,
                               bool requireCapturing,
                               QString *reason);

    // ---- registry API (GUI thread — invokeDeclarativeNetRequest) --
    // extPath/manifest/storeDir come from the calling backend; the
    // registry never touches ExtensionShimBackend itself so tests can
    // drive it directly.
    QJsonObject updateRules(const QString &extensionId,
                            const QString &extensionPath,
                            const QJsonObject &manifest,
                            const QString &storeDir,
                            bool sessionScope,
                            bool withHostAccess,
                            bool feedbackGranted,
                            const QJsonArray &addRules,
                            const QJsonArray &removeRuleIds);
    QJsonArray rulesFor(const QString &extensionId,
                        const QString &extensionPath,
                        const QJsonObject &manifest,
                        const QString &storeDir,
                        bool sessionScope,
                        bool withHostAccess,
                        bool feedbackGranted,
                        const QList<int> &filterIds);
    QJsonObject updateEnabledRulesets(const QString &extensionId,
                                      const QString &extensionPath,
                                      const QJsonObject &manifest,
                                      const QString &storeDir,
                                      bool withHostAccess,
                                      bool feedbackGranted,
                                      const QStringList &enable,
                                      const QStringList &disable);
    QStringList enabledRulesets(const QString &extensionId,
                                const QString &extensionPath,
                                const QJsonObject &manifest,
                                const QString &storeDir,
                                bool withHostAccess,
                                bool feedbackGranted);
    int availableStaticRuleCount(const QString &extensionId,
                                 const QString &extensionPath,
                                 const QJsonObject &manifest,
                                 const QString &storeDir,
                                 bool withHostAccess,
                                 bool feedbackGranted);
    QJsonArray testMatchOutcome(const QString &extensionId,
                                const QString &extensionPath,
                                const QJsonObject &manifest,
                                const QString &storeDir,
                                bool withHostAccess,
                                bool feedbackGranted,
                                const QJsonObject &request);

    // Feedback lane.  matchedRules answers {rulesMatchedInfo:[...]}
    // honoring Chrome's filter {tabId, minTimeStamp}; the 20-calls-
    // per-10min quota is enforced here and returns false when spent.
    bool matchedRulesQuota(const QString &extensionId);
    QJsonArray matchedRules(const QString &extensionId,
                            int tabIdFilter, double minTimeStamp);

    // Extension unload teardown — drops session+dynamic state and the
    // match log; persisted files stay (user data is sacred).
    void dropExtension(const QString &extensionId);

    // GUI-side landing for a match the IO thread recorded.  Public +
    // Q_INVOKABLE so evaluate() can post it across threads.
    Q_INVOKABLE void noteMatch(const QJsonObject &record);

signals:
    // Health-surface notes — the backend mirrors them into its own
    // notes list per extension context that cares.
    void noteRecorded(const QString &message);

public:
    // The normalized rule shape — public because the file-local
    // compilers/matchers in the .cpp keep the parsing readable.
    struct Condition {
        QRegularExpression urlRe;
        bool hasUrlRe = false;
        QSet<QString> initiatorDomains;
        QSet<QString> excludedInitiatorDomains;
        QSet<QString> requestDomains;
        QSet<QString> excludedRequestDomains;
        QSet<QString> requestMethods;
        QSet<QString> excludedRequestMethods;
        int domainType = -1;       // -1 any, 0 firstParty, 1 thirdParty
    };

    struct ParsedRule {
        QString extId;
        QString rulesetId;         // '_session','_dynamic' or static id
        int source = SourceSession;
        int ruleId = 0;
        int order = 0;             // registration order, deterministic
        int priority = 1;
        int action = ActionNone;
        bool unsafe = false;       // regexFilter or regexSubstitution
        // Chrome resource-type names (the evaluator compares the
        // request's mapped type against these — no int enum here so
        // the snapshot stays engine-free).
        QSet<QString> typeNames;
        QSet<QString> excludedTypeNames;
        // redirect target variants
        int redirectKind = 0;      // 0 none,1 url,2 extPath,3 transform,4 regexSub
        QUrl redirectUrl;
        QString redirectExtPath;
        QJsonObject redirectTransform;
        QString regexSubstitution;
        QVector<RequestHeaderOp> headerOps;
        Condition cond;
        QJsonObject json;          // normalized echo for get*Rules
    };

private:
    explicit DeclarativeNetRequest(QObject *parent = nullptr);

    struct RulesetDecl {           // manifest declarative_net_request entry
        QString id;
        QString path;
        bool enabledDefault = false;
    };

    struct ExtState {
        bool loaded = false;
        bool withHostAccess = false;
        bool feedback = false;
        QStringList hostPatterns;
        QVector<ParsedRule> sessionRules;
        QVector<ParsedRule> dynamicRules;
        QList<RulesetDecl> declared;             // manifest order
        QHash<QString, QVector<ParsedRule>> staticRules; // loaded sets
        QSet<QString> staticLoaded;            // file read attempted
        QSet<QString> enabledOverride;         // forced on
        QSet<QString> disabledOverride;        // forced off
        QVector<QJsonObject> matchLog;         // MatchedRuleInfo
        QList<qint64> matchedCalls;            // quota timestamps ms
        int unsafeDynamic = 0;
        int regSeq = 0;
    };

    ExtState &ensureLoaded(const QString &extensionId,
                           const QString &extensionPath,
                           const QJsonObject &manifest,
                           const QString &storeDir,
                           bool withHostAccess, bool feedbackGranted);
    void loadRulesetFile(ExtState &state, const QString &extensionPath,
                         const RulesetDecl &decl);
    QStringList enabledRulesetIds(const ExtState &state) const;
    void persist(const QString &extensionId, const QString &storeDir,
                 ExtState &state);
    void rebuildSnapshot();
    int registeredUnsafeGlobal() const;

    QHash<QString, ExtState> m_exts;
    int m_nextRegSeq = 1;

    // ---- IO-thread lane -------------------------------------------
    struct ExtMeta {
        bool withHostAccess = false;
        bool feedback = false;
        QStringList hostPatterns;
        int regSeq = 0;
    };
    struct SnapRule {
        QString extId;
        ParsedRule rule;
        // CA11/P2: the longest literal run (>=kDnrTokenMin chars) the rule's
        // urlFilter compiles into — "" for regex-only and
        // condition-only rules, which sit in the generic bucket.
        // Every literal char is escaped verbatim into the pattern,
        // so a matching URL must contain it (case-folded: the probe
        // runs on the lowered url, a superset for case-sensitive
        // filters too).
        QString indexToken;
    };
    static QReadWriteLock s_lock;
    static QVector<SnapRule> s_rules;
    static QHash<QString, ExtMeta> s_meta;
    static QAtomicInt s_ruleCount;
    // allowAllRequests exemptions: extId -> normalized frame url ->
    // exempting rule's priority.  Written by evaluate() itself, so a
    // separate mutex keeps the snapshot read path shared-locked.
    static QMutex s_exemptLock;
    static QHash<QString, QHash<QString, int>> s_frameExempt;
};

#endif // DECLARATIVENETREQUEST_H
