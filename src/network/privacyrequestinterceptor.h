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
class PrivacyRequestInterceptor : public QWebEngineUrlRequestInterceptor
{
    Q_OBJECT

public:
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

private:
    AdBlockRequestInterceptor *m_adBlock;
};

#endif // PRIVACYREQUESTINTERCEPTOR_H
