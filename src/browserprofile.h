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

#ifndef BROWSERPROFILE_H
#define BROWSERPROFILE_H

#include <qstring.h>

class QWebEngineProfile;

// Application-wide QWebEngineProfile accessors and the code that
// applies the QSettings "websettings"/"network" groups to a profile.
// They live here so ported modules can use them while
// browserapplication.cpp is still uncompiled — MIG15 delegates
// BrowserApplication::webEngineProfile()/loadSettings() to these.
namespace BrowserProfile {

// The persistent "normal" browsing profile.  QWebEngineProfile::
// defaultProfile() is itself off-the-record in Qt6 (MIG06), so Arora
// browses on the named profile "arora".  Lazily created, qApp-owned.
QWebEngineProfile *normalProfile();

// Lazily created off-the-record profile for private browsing (MIG03):
// nothing — cookies, cache, storage — persists to disk.
QWebEngineProfile *privateProfile();

// TOR02: dedicated off-the-record profile for `arora --tor`.  Also
// unnamed (memory-only), but deliberately distinct from
// privateProfile() so the two modes can never share state and the
// tor profile can carry its own hardening (HTTPS-first interceptor,
// normalized UA, no extensions, no WebChannel bridges).
QWebEngineProfile *torProfile();

// The private profile only if it has already been brought up —
// nullptr otherwise.  For callers that want to adjust an existing
// private session without forcing one into existence (settings
// re-application).
QWebEngineProfile *privateProfileIfCreated();

// UA03: the Chrome milestone the default UA presents on the wire.
// Sites version-sniff "Chrome/<major>" to nag "browser out of date" —
// honestly reporting the bundled Chromium reads stale as soon as its
// milestone lags stable, so the token is bumped to this presentation
// version instead.  Presentation only: the engine underneath is still
// the bundled Chromium and no feature gates change (the escape hatch
// for a site that breaks is the WebPage::setUserAgent override or a
// useragents.xml preset).  Bump the number when sites start flagging
// it as stale again.
int presentedChromeMajor();

// The user agent Arora sends when no override is configured (UA01):
// Qt's factory UA minus the "QtWebEngine/<ver>" product token, which
// bot-detection fingerprints as automation (Google /sorry/ blocks),
// with the Chrome/<ver> token presenting presentedChromeMajor() (UA03).
QString defaultHttpUserAgent();

// UAG01: the effective UA for a context — a non-empty override wins
// verbatim (a switcher preset or the user's custom string), otherwise
// the factory UA is de-badged and bumped.  Under ARORA_RUSTCORE the
// construction runs in rustcore (rc_ua_build); the Qt builder is the
// no-rust implementation, the FFI-failure fallback and the
// parity-test seam.
QString buildHttpUserAgent(const QString &factoryUserAgent,
                           const QString &overrideUserAgent);
QString buildHttpUserAgentQt(const QString &factoryUserAgent,
                             const QString &overrideUserAgent);
// Convenience wrapper — probes the engine's factory UA once.
QString effectiveHttpUserAgent(const QString &overrideUserAgent);

// The Sec-CH-UA full version a UA implies (applyClientHints' input) —
// rc_ua_brand_version under ARORA_RUSTCORE, the Qt version below as
// fallback/parity seam.
QString presentedBrandVersion(const QString &httpUserAgent,
                              const QString &engineVersion);
QString presentedBrandVersionQt(const QString &httpUserAgent,
                                const QString &engineVersion);

// UA02: keeps the profile's UA client hints (Sec-CH-UA*) consistent
// with the UA string actually configured.  Chromium's hints brand the
// engine "Chromium" while our vanilla UA claims "Chrome/<ver>" — real
// Chrome carries a "Google Chrome" brand alongside, so the mismatch
// fingerprints the spoof.  When the effective UA contains "Chrome/",
// the UA's own major is presented on both the "Chromium" and "Google
// Chrome" brands (UA03: the real engine's build tail is kept, which
// is the shape real Chrome uses — a reduced UA over a full build
// version); a non-Chrome UA (e.g. a Firefox preset) resets the hints
// to the honest Chromium defaults.  Call after every setHttpUserAgent.
void applyClientHints(QWebEngineProfile *profile);

// Applies the persisted preferences to the profile's
// QWebEngineSettings (was BrowserApplication::loadSettings() writing
// to QWebSettings::globalSettings()):
//   websettings group  -> fonts, WebAttribute toggles, userStyleSheet
//                         (injected through QWebEngineScript —
//                         QWebSettings::setUserStyleSheetUrl is gone)
//   network group      -> setHttpCacheType/setHttpCacheMaximumSize
//                         and setHttpAcceptLanguage
// Settings with no WebEngine equivalent (DeveloperExtrasEnabled,
// ZoomTextOnly, maximumPagesInCache, the access-keys feature) are
// intentionally not applied; see MIG11 notes in .devin/Arora-Task.md.
void applySettings(QWebEngineProfile *profile);

// REF01: (re)installs the named <meta name="referrer"> injection
// script matching the effective referer level on this profile (the
// tor profile floors at Trimmed).  EngineDefault removes the script.
// applySettings() calls it; standalone callers exist for test modes
// that pin a level after the profile was already prepared.
void installReferrerPolicy(QWebEngineProfile *profile);

// SEC12: QWebEngineProfile exposes no API for DOM storage —
// clearHttpCache() only reaches the HTTP cache.  This schedules
// removal of the per-site storage trees Chromium persists under the
// profile's persistentStoragePath(): localStorage/sessionStorage
// leveldb, IndexedDB, service workers, WebSQL, File System/OPFS,
// blobs, quota bookkeeping, trust tokens, shared dictionaries and
// the persisted network state (HSTS).  Chromium recreates them on
// demand.
// HARD01: deleting any of these trees under a running browser can
// wedge Chromium's storage services and hang the very next
// navigation (bisected: Local Storage, IndexedDB, Service Worker and
// WebStorage each break on their own), so nothing is removed here —
// a sentinel file is left behind and clearDeferredSiteStorage()
// finishes the job at the next profile startup, before any page
// exists.
// Callers should also clear live origins through a page-side script
// sweep (ClearPrivateData does) — in-session reads then return
// empty.  Honest limitation: storage belonging to origins with no
// open page stays on disk (and can be re-read if the site is
// revisited) until the deferred wipe at the next start.
bool clearSiteStorage(QWebEngineProfile *profile);

// PRIV01: stronger variant of the sentinel for the clear-on-exit
// option — additionally schedules the Network/ tree (Chromium's
// cookie database, HSTS, reporting state) and the http cache dirs
// for removal at next start, because the async profile deletes
// (deleteAllCookies/clearHttpCache) are not guaranteed to flush
// before process exit.  Call at quit; nothing is removed in-session.
bool clearAllStorageOnNextStart(QWebEngineProfile *profile);

// HARD01: removes the deferred site-data trees when the sentinel
// from clearSiteStorage() is present.  Runs from normalProfile()
// immediately after profile construction — i.e. only at process
// start, while no WebContents holds the storage services open.
// A no-op without the sentinel.
// PRIV01: also honors the clear-all sentinel from
// clearAllStorageOnNextStart().
bool clearDeferredSiteStorage(const QString &storagePath);

// CONT05: immediate variant of the site-data wipe for a storage root
// whose profile is NOT running — a registered container that was
// never materialized this session (ContainerManager::
// clearUnmaterializedStorage).  Removes the same trees
// clearSiteStorage would defer, right now; safe only because nothing
// holds them open.  Do NOT call on a live profile's root.
bool clearSiteStorageNow(const QString &storagePath);

// PRIV01: appends privacy-motivated Chromium switches to
// QTWEBENGINE_CHROMIUM_FLAGS for QtWebEngineProcess.  Must run before
// the first page spawns the process — main() calls it right after the
// application object exists (profiles are created in its ctor but the
// engine process only starts on the first page).
//   privacy/webrtcIpProtection (default on) ->
//     --force-webrtc-ip-handling-policy=disable_non_proxied_udp
//     WebRTC then only ever runs through the configured proxy — no
//     local, LAN or real WAN address can leak via ICE candidates.
//   privacy/secureDnsMode (0-3, default 0) ->
//     --enable-features=DnsOverHttps when non-zero; see
//     applySecureDns() for the mode meanings.  The legacy PRIV01 bool
//     privacy/secureDns still reads as "automatic" (mode 1).
//   privacy/tlsStrictCiphers (default on) -> TLS01
//     --cipher-suite-blacklist=0x009c,0x009d,0x002f,0x0035,0xc013,0xc014
//     drops the non-forward-secret RSA key-exchange suites and the
//     ECDHE CBC_SHA suites from the ClientHello, leaving TLS 1.3 and
//     ECDHE+AEAD.  Advertised-suites only — negotiated strong suites
//     are unaffected; the escape hatch exists for ancient TLS sites.
//     A --cipher-suite-blacklist already present in the environment
//     takes precedence over the built-in list.
// Flags are process-lifetime — toggling the settings needs a restart.
void applyChromiumFlags();

// DOH01: pushes the privacy/secureDns* settings to
// QWebEngineGlobalSettings::setDnsMode — the Qt6.6+ API for
// Chromium's DnsOverHttpsMode/Templates knobs (the "custom endpoint
// not exposed" bound PRIV01 documented was pre-API).
//   0 Off                       -> SystemOnly (plain system DNS)
//   1 Automatic                 -> SystemOnly + the feature flag
//     auto-upgrades when the network resolver is a known provider —
//     the provider list is Chromium's, not configurable.
//   2 Custom (with fallback)    -> SecureWithFallback + the
//     privacy/secureDnsServer URI template; plain DNS still answers
//     when the endpoint fails.
//   3 Custom (strict)           -> SecureOnly + the template; nothing
//     falls back to plain DNS, so an unreachable endpoint means no
//     resolution at all.
// Empty/garbage templates resolve to the default
// (https://cloudflare-dns.com/dns-query) rather than silently
// downgrading the strict mode to plaintext DNS.
// Engine-global and safe to re-call — invoked from applySettings().
void applySecureDns();

// PRIV02: fingerprint-normalization that works through the process
// environment rather than a Chromium switch.  When
// privacy/reportUtcTimezone is on, TZ is forced to UTC before the
// engine exists: Chromium detects the host timezone in-process (the
// "browser process" lives inside the app) and every spawned
// QtWebEngineProcess inherits the environment, so JS Date/Intl reads
// report UTC.  The caller's own TZ value is remembered and restored
// when the toggle is switched back off.
// Call from main() BEFORE the application object exists (its ctor
// already touches the profile) and again from the settings dialog so
// later-spawned engine processes pick up a mid-session flip — engine
// processes already running keep the zone they started with, hence
// the restart note on the checkbox.
void applyFingerprintEnvironment();

// SEC12: force a profile data tree owner-only — 0700 directories,
// 0600 files.  Chromium already creates them that way; this repairs
// trees loosened by umask quirks or manual copies.  Returns false if
// any permission could not be fixed.
bool ensureUserOnlyPermissions(const QString &path);

} // namespace BrowserProfile

#endif // BROWSERPROFILE_H
