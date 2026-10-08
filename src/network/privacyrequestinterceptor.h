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
class QByteArray;
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
// Referer policy (REF01): Chromium's built-in default is
// strict-origin-when-cross-origin — same-site requests carry the full
// URL (path + query leak) and cross-site requests still reveal the
// *referring* site's origin.  applyRefererPolicy() rewrites the
// renderer-computed Referer — which is present in httpHeaders() for
// every request class, and whose setHttpHeader() override DOES reach
// the wire for subresources on Qt 6.12 (verified against a loopback
// two-site matrix — the PRIV01 comment that called subresource writes
// a no-op was stale).  An absent header stays absent: rel=noreferrer
// links, pages with a stricter Referer-Policy and https->http
// downgrades are never handed a referer the sender asked to withhold.
// One leg the rewrite cannot reach: redirect follow-ups recompute the
// Referer from the redirect chain's stored referrer AFTER the
// interceptor ran — those are covered by the page-level
// referrer-meta script BrowserProfile installs (referrerMetaValue).
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
//
// PING01: privacy/blockPings (default on) drops the telemetry-shaped
// request types — sendBeacon beacons, <a ping> hyperlink audits and
// CSP violation reports all ride Chromium's ping/report upload
// channel.  BrowserProfile additionally switches
// HyperlinkAuditingEnabled off so <a ping> requests never initiate.
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

    // REF01: persisted as privacy/refererPolicy (int), migrated from
    // the PRIV01 privacy/trimReferer bool (false -> EngineDefault,
    // anything else -> the Trimmed default).
    //   EngineDefault  untouched — Chromium strict-origin-when-cross-origin
    //   Trimmed        cross-site -> the *target's* own origin (uBO
    //                  referrer-spoof trick: nothing about the source
    //                  page crosses); same-site -> the referrer's
    //                  origin only, never path/query.  Redirect
    //                  follow-ups still carry the source ORIGIN —
    //                  Chromium recomputes them past the interceptor.
    //   Strict         cross-site -> no Referer at all (the injected
    //                  meta covers redirect legs too); same-site ->
    //                  origin only
    //   Never          the header is removed everywhere
    enum RefererPolicy {
        RefererEngineDefault = 0,
        RefererTrimmed = 1,
        RefererStrict = 2,
        RefererNever = 3
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
    static int refererPolicy();
    static bool isUpgradeCandidate(const QUrl &url);
    static void clearDowngradedHosts();  // test cleanup

    // The persisted level straight from QSettings incl. the
    // trimReferer-bool migration — usable before loadSettings() has
    // populated the snapshot (BrowserProfile consults it when
    // installing the referrer-meta script).
    static int storedRefererPolicy();

    // REF01: Chromium recomputes the Referer for redirect follow-up
    // legs *after* the interceptor ran, so setHttpHeader never reaches
    // their wire value.  The reachable fix is a page-level referrer
    // policy — the source document's policy is what the redirect
    // chain carries.  Maps a level to the <meta name="referrer">
    // content injected into pages that don't set their own:
    //   Trimmed -> "strict-origin"  (path/query never leaves on any
    //                              leg; the interceptor's target-
    //                              origin spoof still covers the legs
    //                              it can write)
    //   Strict  -> "same-origin"    (cross-site gets nothing, incl.
    //                              redirect legs)
    //   Never   -> "no-referrer"
    // EngineDefault -> "" (no script).
    static QByteArray referrerMetaValue(int level);

    // REF01: apply the loaded referer policy to one request.  Safe on
    // the IO thread (reads the lock-guarded snapshot).  minimumLevel
    // lets the tor profile enforce at least RefererTrimmed even when
    // the user picked EngineDefault for the normal profiles.
    static void applyRefererPolicy(QWebEngineUrlRequestInfo &info,
                                   int minimumLevel = RefererEngineDefault);

    // The pure decision behind applyRefererPolicy(): the Referer value
    // that should go on the wire for a request to `target` whose
    // renderer-computed referer is `source`.  An empty return means
    // the header must be removed.  EngineDefault returns `source`
    // unchanged.
    static QByteArray rewrittenReferer(int level, const QUrl &source,
                                       const QUrl &target);

    // SECLVL: the tier currently loaded into the IO-thread snapshot
    // and the pure decision the interceptor consults — block a
    // script-execution request whose first-party page is insecure
    // http (loopback exempt).  shouldBlockScript also answers for
    // Safest: the engine-side JavascriptEnabled-off is stronger, but
    // the rule still holds when a page-level JS override ever lands.
    static int securityLevel();
    static bool shouldBlockScript(const QUrl &firstPartyUrl,
            QWebEngineUrlRequestInfo::ResourceType type);

    // PING01: the persisted privacy/blockPings toggle (default on) as
    // loaded into the IO-thread snapshot — sendBeacon/<a ping>/CSP
    // report requests are dropped while it is set.  The tor
    // interceptor consults the same toggle.
    static bool blockPingsEnabled();

private:
    AdBlockRequestInterceptor *m_adBlock;
};

#endif // PRIVACYREQUESTINTERCEPTOR_H
