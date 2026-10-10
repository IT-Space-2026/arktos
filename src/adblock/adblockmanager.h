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

#ifndef ADBLOCKMANAGER_H
#define ADBLOCKMANAGER_H

#include <qobject.h>

#include <qpointer.h>

class QUrl;
class QWidget;
class QWebEngineProfile;
class AutoSaver;
class AdBlockDialog;
class AdBlockNetwork;
class AdBlockPage;
class AdBlockSubscription;
class AdBlockManager : public QObject
{
    Q_OBJECT

signals:
    void rulesChanged();

public:
    // TELEM01: remote filter-list downloads are consent-gated — the
    // seeded subscriptions must not make the browser contact
    // easylist.to/ublockorigin.github.io on its own.  Undecided asks
    // once on first launch (maybePromptForListConsent); explicit user
    // actions — accepting an abp: subscribe link, pressing Update
    // Subscription, toggling the blocker on — grant consent through
    // grantRemoteLists()/setRemoteListsConsent.
    enum RemoteListsConsent {
        RemoteListsUndecided = -1,
        RemoteListsDeclined = 0,
        RemoteListsGranted = 1
    };
    static RemoteListsConsent remoteListsConsent();
    static void setRemoteListsConsent(RemoteListsConsent consent);
    // True while remote subscription fetches are allowed (consent
    // granted).  Consulted by AdBlockSubscription's automatic update
    // trigger; updateNow() itself stays an explicit fetch.
    static bool remoteListsAllowed();
    // Grants consent and refreshes every stale remote subscription.
    void grantRemoteLists();
    // One-time first-launch prompt; a no-op once a choice is stored,
    // when the blocker is disabled, or when no remote subscription
    // exists.
    void maybePromptForListConsent(QWidget *parent);

    AdBlockManager(QObject *parent = nullptr);
    ~AdBlockManager();

    void load();

    static AdBlockManager *instance();
    bool isEnabled() const;

    QList<AdBlockSubscription*> subscriptions() const;
    void removeSubscription(AdBlockSubscription *subscription);
    void addSubscription(AdBlockSubscription *subscription);

    // ADB03: preset-catalog helpers (adblockpresets.h lists what the
    // presets dialog offers).  subscriptionForLocation matches a list
    // by its real URL — not the abp:subscribe wrapper — so presets
    // and hand-subscribed lists dedupe.  subscribeRemoteList adds the
    // subscription when missing and re-enables a disabled one; ticking
    // the preset box is the user's explicit consent to download remote
    // lists (TELEM01), so consent is granted and a fetch is kicked.
    AdBlockSubscription *subscriptionForLocation(const QUrl &location) const;
    AdBlockSubscription *subscribeRemoteList(const QUrl &location,
                                             const QString &title);

    AdBlockNetwork *network();
    AdBlockPage *page();
    AdBlockSubscription *customRules();

    // SHLD01: per-site content-blocking switch, surfaced by the
    // location-bar shield panel.  Whitelisting a site writes the
    // standard ABP "@@||host^$document" exception into the user's
    // custom-rules subscription — the matcher's document-exception
    // path then allows every request the page makes and AdBlockPage
    // skips its cosmetic injection.  Persists through the normal
    // subscription save path and works under both the native and the
    // optional Rust engine.
    // siteWhitelistFilter returns an empty string for hosts that can
    // not be expressed safely inside an ABP domain anchor.
    // siteWhitelistKey is the canonical host the SITED01 store keys
    // the whitelist row by (ACE/lowercase, same validation).
    static QString siteWhitelistFilter(const QString &host);
    static QString siteWhitelistKey(const QString &host);
    bool isSiteWhitelisted(const QString &host);
    void setSiteWhitelisted(const QString &host, bool whitelisted);

    // Installs the AdBlockRequestInterceptor on the profile; WebEngine
    // routes all page loads through it instead of the application QNAM.
    // With deferInitialRules (PERF03) the matcher's first rules
    // snapshot — a full parse of every subscribed list — is left to
    // the caller to queue after the first window is shown; until that
    // rebuild runs the interceptor sees an empty ruleset (allow-all).
    void installOnProfile(QWebEngineProfile *profile,
                          bool deferInitialRules = false);

    // ADB06: which matcher answers requests, picked at runtime in
    // Preferences (persisted as AdBlock/engine = "cpp"|"rust").
    // RustEngine is Brave's adblock-rust — reachable only in
    // CONFIG+=adblock_rust builds, where it still constructs lazily on
    // first selection; every other configuration is NativeEngine.
    // storedEngine() returns the raw persisted pick so a non-rust
    // build can round-trip a "rust" choice instead of silently
    // rewriting it; engine() is the effective pick and clamps to
    // NativeEngine when the Rust engine is not compiled in.
    enum Engine {
        NativeEngine = 0,
        RustEngine = 1
    };
    static bool rustEngineAvailable();
    static Engine storedEngine();
    Engine engine() const;
    // True while the live matcher snapshot delegates to the Rust
    // engine.  Never constructs the network snapshot just to answer.
    bool rustEngineInUse() const;

public slots:
    void setEnabled(bool enabled);
    // Persists the pick and emits rulesChanged, so the matcher
    // snapshot is rebuilt and the switch applies to the next request —
    // no restart.
    void setEngine(Engine engine);
    AdBlockDialog *showDialog();

private slots:
    void save();

private:
    static QUrl customSubscriptionUrl();
    static AdBlockManager *s_adBlockManager;

    bool m_loaded;
    bool m_enabled;
    AutoSaver *m_saveTimer;
    QPointer<AdBlockDialog> m_adBlockDialog;
    AdBlockNetwork *m_adBlockNetwork;
    AdBlockPage *m_adBlockPage;
    QList<AdBlockSubscription*> m_subscriptions;

};

#endif // ADBLOCKMANAGER_H

