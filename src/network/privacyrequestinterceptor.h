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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

#ifndef PRIVACYREQUESTINTERCEPTOR_H
#define PRIVACYREQUESTINTERCEPTOR_H

#include <qwebengineurlrequestinterceptor.h>
#include <qwebengineurlrequestinfo.h>

class AdBlockNetwork;
class AdBlockRequestInterceptor;
class QString;
class QUrl;

// PRIV01: the browsing profile's request interceptor.  A profile
// accepts exactly one QWebEngineUrlRequestInterceptor, so the privacy
// stages and the adblock matcher are composed here — plain http:
// main-frame navigations are upgraded to https: and cross-site
// referrers are trimmed before the adblock rules run.  The tor
// profile uses the stricter TorRequestInterceptor instead (it
// upgrades every request, not just navigations).
//
// HTTPS-First: upgrading can break sites that only serve plain http.
// When an upgraded (or directly requested) https main-frame load
// fails, WebPage reports it through noteNavigationFailure() and the
// host joins a session-scoped downgrade set — subsequent http:
// requests to it pass through un-upgraded, and the error page tells
// the user.  Nothing is persisted.
//
// Referrer trim: Chromium already defaults to
// strict-origin-when-cross-origin, which still sends the *referring
// site's origin* to third parties.  When enabled, cross-site requests
// instead carry the *target's* own origin — nothing about the page
// the user came from leaks at all (same trick as uBlock Origin's
// referrer spoof / Firefox's target-origin trimming).
//
// SECLVL: Mullvad-style security tiers (privacy/securityLevel).
// From Safer up, script-execution subresource requests (external
// scripts, workers, service workers) are dropped when the page's own
// origin is insecure http — the interceptor cannot reach inline
// <script> blocks or event handlers, which still run (that half of
// the tier is documented in the Settings hint; full scriptless pages
// are Safest's JavascriptEnabled-off job).  http on loopback is a
// "potentially trustworthy" secure context in Chromium, so local dev
// pages keep their scripts; plain http on LAN/public hosts does not.
class PrivacyRequestInterceptor : public QWebEngineUrlRequestInterceptor
{
    Q_OBJECT

public:
    // Persisted as privacy/securityLevel (int).  Standard is the
    // default = current behavior; the engine-side halves live in
    // BrowserProfile::applySettings().
    enum SecurityLevel {
        Standard = 0,
        Safer = 1,
        Safest = 2
    };


    PrivacyRequestInterceptor(AdBlockNetwork *network, QObject *parent = nullptr);

    void interceptRequest(QWebEngineUrlRequestInfo &info) override;

    // GUI thread: refresh the IO-thread policy snapshot from the
    // "privacy" QSettings group.  Called from BrowserProfile::
    // applySettings() (which runs at profile setup and on every
    // settings-dialog save).
    static void loadSettings();

    // GUI thread: an https main-frame load failed.  Records the host
    // in the session downgrade set (no-op when https-first is off)
    // and returns true when this call newly downgraded it.
    static bool noteNavigationFailure(const QUrl &url);

    // Whether http: requests to host currently skip the upgrade —
    // either because it was downgraded after a failed https load, or
    // because it was never a candidate (loopback/private/local).
    static bool isDowngraded(const QString &host);

    // Test/introspection seam: the static policy decisions.
    static bool httpsFirstEnabled();
    static bool trimRefererEnabled();
    static bool isUpgradeCandidate(const QUrl &url);
    static void clearDowngradedHosts();  // test cleanup

    // SECLVL: the tier currently loaded into the IO-thread snapshot
    // and the pure decision the interceptor consults — block a
    // script-execution request whose first-party page is insecure
    // http (loopback exempt).  shouldBlockScript also answers for
    // Safest: the engine-side JavascriptEnabled-off is stronger, but
    // the rule still holds when a page-level JS override ever lands.
    static int securityLevel();
    static bool shouldBlockScript(const QUrl &firstPartyUrl,
            QWebEngineUrlRequestInfo::ResourceType type);

private:
    AdBlockRequestInterceptor *m_adBlock;
};

#endif // PRIVACYREQUESTINTERCEPTOR_H
