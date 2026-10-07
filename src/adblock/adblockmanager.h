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
    static QString siteWhitelistFilter(const QString &host);
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

public slots:
    void setEnabled(bool enabled);
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

