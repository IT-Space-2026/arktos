/*
 * Copyright 2026 The Arora Authors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

#include "scriptcontrolmanager.h"

#include "privacyrequestinterceptor.h"

#include <qapplication.h>
#include <qreadwritelock.h>
#include <qsettings.h>
#include <qurl.h>
#include <qwebengineurlrequestinfo.h>

// The interceptor callback runs on Chromium's IO thread — the allowed
// hosts are mirrored into this lock-guarded snapshot on every rule
// change, so interceptRequest never touches the GUI-side state.
static QReadWriteLock s_snapshotLock;
static QStringList s_allowedSnapshot;

#if defined(ARORA_RUSTCORE)
#include "sitedecisionstore.h"

// SITED01: one-shot import of the legacy "scriptcontrol" QSettings
// group into the Rust store; the keys are retired after the writes
// land so an interrupted run re-imports on next start.
static void importLegacyScriptRules()
{
    QSettings settings;
    // Same normalization the loader applies: lowercase, trim edge
    // dots, drop empties.
    const auto normalize = [](const QString &host) {
        QString normalized = host.toLower();
        while (normalized.endsWith(QLatin1Char('.')))
            normalized.chop(1);
        while (normalized.startsWith(QLatin1Char('.')))
            normalized = normalized.mid(1);
        return normalized;
    };
    settings.beginGroup(QLatin1String("scriptcontrol"));
    const QStringList allowed =
        settings.value(QLatin1String("allowed")).toStringList();
    const QStringList blocked =
        settings.value(QLatin1String("blocked")).toStringList();
    if (allowed.isEmpty() && blocked.isEmpty()) {
        settings.endGroup();
        return;
    }
    bool allWritten = true;
    for (const QString &host : allowed) {
        const QString normalized = normalize(host);
        if (!normalized.isEmpty())
            allWritten = SiteDecisionStore::set(
                SiteDecisionStore::KindJavaScript,
                normalized, QLatin1String("allow")) && allWritten;
    }
    for (const QString &host : blocked) {
        const QString normalized = normalize(host);
        if (!normalized.isEmpty())
            allWritten = SiteDecisionStore::set(
                SiteDecisionStore::KindJavaScript,
                normalized, QLatin1String("block")) && allWritten;
    }
    settings.endGroup();
    if (allWritten)
        settings.remove(QLatin1String("scriptcontrol"));
}

// The in-memory lists are authoritative; mirror them into the store
// in one whole-kind write.  A host can never be in both lists (a
// mutation removes it from the other first), but on a hand-edited
// file block wins — the conservative answer.
static void persistScriptRules(const QStringList &allowed,
                               const QStringList &blocked)
{
    QHash<QString, QString> rows;
    for (const QString &host : allowed)
        rows.insert(host, QLatin1String("allow"));
    for (const QString &host : blocked)
        rows.insert(host, QLatin1String("block"));
    SiteDecisionStore::replace(SiteDecisionStore::KindJavaScript, rows);
}
#endif

ScriptControlManager::ScriptControlManager(QObject *parent)
    : QObject(parent)
{
#if defined(ARORA_RUSTCORE)
    importLegacyScriptRules();
    const QHash<QString, QString> rows =
        SiteDecisionStore::entries(SiteDecisionStore::KindJavaScript);
    for (auto it = rows.constBegin(); it != rows.constEnd(); ++it) {
        const QString normalized = normalizeHost(it.key());
        if (normalized.isEmpty())
            continue;
        if (it.value() == QLatin1String("allow")) {
            if (!m_allowed.contains(normalized))
                m_allowed.append(normalized);
        } else if (it.value() == QLatin1String("block")) {
            if (!m_blocked.contains(normalized))
                m_blocked.append(normalized);
        }
        // Unknown values are corrupt rows — dropped on load and gone
        // after the next mirror-write.
    }
#else
    QSettings settings;
    settings.beginGroup(QLatin1String("scriptcontrol"));
    const QStringList allowed =
        settings.value(QLatin1String("allowed")).toStringList();
    const QStringList blocked =
        settings.value(QLatin1String("blocked")).toStringList();
    settings.endGroup();
    for (const QString &host : allowed) {
        const QString normalized = normalizeHost(host);
        if (!normalized.isEmpty() && !m_allowed.contains(normalized))
            m_allowed.append(normalized);
    }
    for (const QString &host : blocked) {
        const QString normalized = normalizeHost(host);
        if (!normalized.isEmpty() && !m_blocked.contains(normalized))
            m_blocked.append(normalized);
    }
#endif
    refreshSnapshot();
}

ScriptControlManager *ScriptControlManager::instance()
{
    static ScriptControlManager *manager = new ScriptControlManager(qApp);
    return manager;
}

QString ScriptControlManager::normalizeHost(const QString &host)
{
    QString normalized = host.toLower();
    while (normalized.endsWith(QLatin1Char('.')))
        normalized.chop(1);
    // A leading dot is meaningless in this store — the suffix match
    // already covers "example.com or any subdomain".
    while (normalized.startsWith(QLatin1Char('.')))
        normalized = normalized.mid(1);
    return normalized;
}

// CookieJar-style suffix matching: "example.com" governs
// "example.com" itself and every "www.example.com" under it.
bool ScriptControlManager::matchesHost(const QStringList &list,
                                       const QString &host)
{
    for (const QString &rule : list) {
        if (host == rule || host.endsWith(QLatin1Char('.') + rule))
            return true;
    }
    return false;
}

// The length of the longest rule entry matching host, or -1 — an
// exact rule on "www.example.com" is more specific than a parent rule
// on "example.com", and specificity decides conflicts.
int ScriptControlManager::bestMatchLength(const QStringList &list,
                                          const QString &host)
{
    int best = -1;
    for (const QString &rule : list) {
        if ((host == rule || host.endsWith(QLatin1Char('.') + rule))
            && int(rule.length()) > best)
            best = rule.length();
    }
    return best;
}

int ScriptControlManager::bestMatchLength(const QSet<QString> &set,
                                          const QString &host)
{
    int best = set.contains(host) ? int(host.length()) : -1;
    QString parent = host;
    int dot = parent.indexOf(QLatin1Char('.'));
    while (dot >= 0) {
        parent = parent.mid(dot + 1);
        if (set.contains(parent) && int(parent.length()) > best)
            best = parent.length();
        dot = parent.indexOf(QLatin1Char('.'));
    }
    return best;
}

ScriptControlManager::Rule ScriptControlManager::ruleForHost(
        const QString &host) const
{
    const QString normalized = normalizeHost(host);
    if (normalized.isEmpty())
        return SiteDefault;

    const int sessionBlock = bestMatchLength(m_sessionBlocked, normalized);
    const int sessionAllow = bestMatchLength(m_sessionAllowed, normalized);
    const int block = bestMatchLength(m_blocked, normalized);
    const int allow = bestMatchLength(m_allowed, normalized);

    // The most specific matching rule wins — "Always allow" on
    // www.example.com must lift a Block recorded on the parent
    // example.com.  At equal specificity the session overlay is the
    // newer gesture ("Allow once" beats a persistent rule); within a
    // layer a same-specificity conflict resolves to deny, the
    // conservative answer for a hand-edited settings file.
    const int best = qMax(qMax(sessionBlock, sessionAllow),
                          qMax(block, allow));
    if (best < 0)
        return SiteDefault;
    const bool sessionWins =
        qMax(sessionBlock, sessionAllow) == best;
    const int denyScore = sessionWins ? sessionBlock : block;
    const int allowScore = sessionWins ? sessionAllow : allow;
    return denyScore >= allowScore ? Block : Allow;
}

void ScriptControlManager::setRuleForHost(const QString &host, Rule rule,
                                          bool persistent)
{
    const QString normalized = normalizeHost(host);
    if (normalized.isEmpty())
        return;

    if (persistent) {
        // Exact entries only: a rule on a parent domain is left alone,
        // like CookieJar's clearRuleForHost — removing it here would
        // silently change the decision for sibling hosts.
        m_allowed.removeAll(normalized);
        m_blocked.removeAll(normalized);
        if (rule == Allow)
            m_allowed.append(normalized);
        else if (rule == Block)
            m_blocked.append(normalized);

#if defined(ARORA_RUSTCORE)
        persistScriptRules(m_allowed, m_blocked);
#else
        QSettings settings;
        settings.beginGroup(QLatin1String("scriptcontrol"));
        if (m_allowed.isEmpty())
            settings.remove(QLatin1String("allowed"));
        else
            settings.setValue(QLatin1String("allowed"), m_allowed);
        if (m_blocked.isEmpty())
            settings.remove(QLatin1String("blocked"));
        else
            settings.setValue(QLatin1String("blocked"), m_blocked);
        settings.endGroup();
#endif
    } else {
        m_sessionAllowed.remove(normalized);
        m_sessionBlocked.remove(normalized);
        if (rule == Allow)
            m_sessionAllowed.insert(normalized);
        else if (rule == Block)
            m_sessionBlocked.insert(normalized);
    }

    refreshSnapshot();
    emit changed();
}

void ScriptControlManager::clearPersistentRules()
{
    m_allowed.clear();
    m_blocked.clear();
#if defined(ARORA_RUSTCORE)
    SiteDecisionStore::clear(SiteDecisionStore::KindJavaScript);
#else
    QSettings settings;
    settings.beginGroup(QLatin1String("scriptcontrol"));
    settings.remove(QLatin1String("allowed"));
    settings.remove(QLatin1String("blocked"));
    settings.endGroup();
#endif
    refreshSnapshot();
    emit changed();
}

void ScriptControlManager::clearSessionRules()
{
    m_sessionAllowed.clear();
    m_sessionBlocked.clear();
    refreshSnapshot();
    emit changed();
}

void ScriptControlManager::refreshSnapshot() const
{
    QStringList snapshot = m_allowed;
    for (const QString &host : m_sessionAllowed) {
        if (!snapshot.contains(host))
            snapshot.append(host);
    }
    QWriteLocker lock(&s_snapshotLock);
    s_allowedSnapshot = snapshot;
}

bool ScriptControlManager::isAllowedHostSnapshot(const QString &host)
{
    const QString normalized = normalizeHost(host);
    if (normalized.isEmpty())
        return false;
    QReadLocker lock(&s_snapshotLock);
    return matchesHost(s_allowedSnapshot, normalized);
}

// Schemes that carry application chrome — they keep JavaScript at
// every tier so the start page, DevTools and our own scheme pages
// keep working even at Safest.
static bool isInternalScheme(const QString &scheme)
{
    static const QSet<QString> schemes = {
        QStringLiteral("about"),
        QStringLiteral("arora-cert-error"),
        QStringLiteral("arora-file"),
        QStringLiteral("arora-resource"),
        QStringLiteral("abp"),
        QStringLiteral("chrome"),
        QStringLiteral("chrome-extension"),
        QStringLiteral("devtools"),
        QStringLiteral("qrc"),
    };
    return schemes.contains(scheme);
}

bool ScriptControlManager::isJavaScriptEnabledFor(const QUrl &url) const
{
    const QString host = normalizeHost(url.host());
    if (!host.isEmpty()) {
        const Rule rule = ruleForHost(host);
        // An explicit choice beats every automatic rule — an Allow
        // grant re-enables scripts even under Safest, a Block kills
        // them even under Standard.
        if (rule == Allow)
            return true;
        if (rule == Block)
            return false;
    }

    const QString scheme = url.scheme().toLower();
    if (isInternalScheme(scheme))
        return true;

    if (PrivacyRequestInterceptor::securityLevel()
            >= PrivacyRequestInterceptor::Safest)
        return false;
    // Safer treats an insecure http origin as scriptless; the
    // interceptor's own decision encodes that (incl. the loopback
    // secure-context exemption), so reuse it rather than duplicating.
    if (PrivacyRequestInterceptor::shouldBlockScript(
            url, QWebEngineUrlRequestInfo::ResourceTypeScript))
        return false;

    // The profile-level user preference is the baseline.  Note the
    // Allow check already returned above — a grant overrides this too.
    return QSettings().value(QLatin1String("websettings/enableJavascript"),
                             true).toBool();
}
