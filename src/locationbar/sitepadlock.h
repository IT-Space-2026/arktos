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

#ifndef SITEPADLOCK_H
#define SITEPADLOCK_H

#include <qpointer.h>
#include <qtoolbutton.h>
#include <qurl.h>

class QFrame;
class QLabel;
class QMenu;
class QVBoxLayout;
class WebView;

// TRUST14: the URL-bar padlock — the small lock that took the shield's
// in-bar slot when the shield moved onto the navigation toolbar.  It
// summarizes the connection and the site-trust verdict at a glance:
//
//   https + fully trusted   -> green locked padlock
//      (TLS Verified by the rustls second-opinion probe AND a positive
//      trust verdict — a Tranco-ranked or otherwise positively
//      verified host; a clean chain alone is not enough)
//   https + neutral         -> yellow locked padlock
//      (no negative signal: TLS Verified/Unverified/None, amber or no
//      verdict — the "not untrusted, not trusted / no info" tier)
//   https + untrusted       -> red locked padlock
//      (TLS Warning, threat-list hit or lookalike hit)
//   not https               -> unlocked padlock in the same tier color
//      (http pages keep the verdict's color — a flagged site still
//      shows red — while internal/local pages carry no verdict)
//
// Honesty rules inherited from TlsVerifier: a Pending probe renders
// neutral/yellow, never red; tor/proxied pages are never probed and
// can therefore never reach green — they honestly show "no probe"
// neutral rather than a misleading trusted mark.  Chromium's own
// cert-error interstitial still owns the invalid-https UX; the padlock
// only reflects the verdict.
//
// BrowserApplication::siteTrustEnabled() gates the whole trust
// surface: off means the padlock makes NO trust claim at all — it
// falls back to a plain neutral chrome lock that still reports the
// scheme honestly (locked = https, open = otherwise) rather than
// disappearing and taking the secure/insecure signal with it.
//
// The click opens a small info-only popup (scheme, TLS status, trust
// reason) — there is deliberately no certificate bypass affordance.
class SitePadlockButton : public QToolButton
{
    Q_OBJECT

public:
    // The one state the icon, tooltip and popup all derive from —
    // ordered so tests can read it directly.
    enum class State {
        LockedGreen,
        LockedYellow,
        LockedRed,
        UnlockedGreen,
        UnlockedYellow,
        UnlockedRed,
        PlainLocked,    // https while the trust surface is off
        PlainUnlocked   // no site to evaluate, or trust off + not https
    };
    Q_ENUM(State)

    explicit SitePadlockButton(QWidget *parent = nullptr);
    void setWebView(WebView *webView);

    // The whole decision table as a pure function of the page url —
    // the icon/tooltip/popup render it and the autotest exercises it.
    static State stateFor(const QUrl &url);

    State state() const { return m_state; }

    // Test seam — the info popup's content frame (inside the menu).
    QFrame *popup() const { return m_popup; }

public slots:
    // Re-read the page url and re-render icon, tooltip and popup —
    // public so the main window can re-evaluate the active tab's
    // padlock on tab switches (a trust-flag flip applies on the next
    // refresh; consumers read the QSettings keys live).
    void refresh();

private:
    void rebuildPopup();
    QIcon makeIcon(State state) const;

    QPointer<WebView> m_webView;
    QMenu *m_menu;
    QFrame *m_popup;
    QLabel *m_popupHost;
    QLabel *m_popupSecurity;
    QLabel *m_popupTls;
    QWidget *m_popupTrustBox;
    QVBoxLayout *m_popupTrustLayout;
    State m_state = State::PlainUnlocked;
};

#endif // SITEPADLOCK_H
