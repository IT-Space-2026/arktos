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

#ifndef SCRIPTCONTROLMANAGER_H
#define SCRIPTCONTROLMANAGER_H

#include <qobject.h>
#include <qset.h>
#include <qstring.h>
#include <qstringlist.h>

class QUrl;

// JSCTL: per-site JavaScript control.
//
// The SEC05 permission broker is keyed on QWebEnginePermission::
// PermissionType, which has no JavaScript feature, so the allow/deny
// grants live in their own QSettings map (group "scriptcontrol",
// lists "allowed"/"blocked", normalized lowercase hosts).  Two kinds
// of entries exist:
//
//   - persistent rules, written to QSettings and audited through the
//     Settings dialog's Site Permissions list;
//   - session rules, held in memory only — off-the-record pages never
//     persist (same discipline as SEC05) and the info bar's "Allow
//     once" lands here too.
//
// A rule recorded on a parent domain covers subdomains ("example.com"
// governs "www.example.com"), matching CookieJar's exception
// semantics.  The most specific matching rule wins; at equal
// specificity the session overlay beats the persistent store, and
// deny wins a same-layer conflict.
//
// Effective decision for a navigation, in precedence order (explicit
// choice beats every automatic rule):
//
//   Allow rule                     -> scripts run
//   Block rule                     -> scripts blocked
//   Safest tier (SECLVL)           -> scripts blocked
//   Safer tier + insecure http     -> scripts blocked
//   websettings/enableJavascript=0 -> scripts blocked
//   otherwise                      -> scripts run
//
// Internal schemes (qrc:, arora-*, about:, chrome:, devtools:, ...) are
// always allowed — they carry application pages that need JavaScript
// even at Safest.  The decision is applied through the per-page
// QWebEngineSettings::JavascriptEnabled attribute (per-page settings
// really are per-page in Qt6 — verified against 6.12), so an allowed
// page runs scripts even while the profile-wide tier is Safest.
class ScriptControlManager : public QObject
{
    Q_OBJECT

public:
    enum Rule {
        SiteDefault = 0,    // no entry — tier and global pref decide
        Allow,              // run JavaScript even when a tier blocks it
        Block               // never run JavaScript for this host
    };
    Q_ENUM(Rule)

    explicit ScriptControlManager(QObject *parent = nullptr);

    // Lazy qApp-owned singleton (HistoryManager pattern); the class is
    // still directly constructible for autotests.
    static ScriptControlManager *instance();

    // The effective rule for host — session overlay first, then the
    // persistent lists; both match a host against its parent-domain
    // entries.  SiteDefault when nothing matches.
    Rule ruleForHost(const QString &host) const;

    // Records a rule.  persistent=false keeps it in the session
    // overlay — that is what "Allow once" and off-the-record pages use.
    // SiteDefault removes every rule for the host.
    void setRuleForHost(const QString &host, Rule rule,
                        bool persistent = true);

    // The persistent lists, for the Settings audit list.
    QStringList allowedHosts() const { return m_allowed; }
    QStringList blockedHosts() const { return m_blocked; }
    void clearPersistentRules();

    // Final script decision for a main-frame navigation — folds the
    // per-site rules over the security tier and the global
    // enableJavascript preference, in the precedence order documented
    // above.
    bool isJavaScriptEnabledFor(const QUrl &url) const;

    // IO-thread-safe check for PrivacyRequestInterceptor: hosts with an
    // Allow rule must keep fetching script subresources — the Safer
    // http drop would otherwise kill the very scripts the grant just
    // re-enabled.  Reads a lock-guarded snapshot refreshed on every
    // rule change; never touches the GUI-side lists.
    static bool isAllowedHostSnapshot(const QString &host);

    // Test hook: drop the session overlay (persistent rules stay).
    void clearSessionRules();

signals:
    // Emitted when any rule changes so open panels/lists can refresh.
    void changed();

private:
    static QString normalizeHost(const QString &host);
    static bool matchesHost(const QStringList &list, const QString &host);
    static int bestMatchLength(const QStringList &list, const QString &host);
    static int bestMatchLength(const QSet<QString> &set, const QString &host);
    void refreshSnapshot() const;

    QStringList m_allowed;
    QStringList m_blocked;
    QSet<QString> m_sessionAllowed;
    QSet<QString> m_sessionBlocked;
};

#endif // SCRIPTCONTROLMANAGER_H
