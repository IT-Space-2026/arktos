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

#ifndef SITEPANEL_H
#define SITEPANEL_H

#include <qcolor.h>
#include <qframe.h>
#include <qlist.h>
#include <qpointer.h>
#include <qstringlist.h>
#include <qurl.h>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QVBoxLayout;
class CookieJar;
class WebView;

// SHLD01: per-site privacy panel — the body of the location-bar
// shield button's popup (hosted inside a QMenu via QWidgetAction).
// Everything is scoped to the page's current host:
//
//   - security state line (https / http / cert-error / local),
//   - a TRUST08 Trust section listing the signals rustcore's
//     composite site-trust verdict (rc_sitetrust_lookup) evaluated
//     for the host — threat-list hit, lookalike resemblance, Tranco
//     rank and first visit — only the signals that fired or are
//     informative get a line; the whole section hides on pages with
//     nothing to say, and TRUST03's trust/* switches gate the lookup
//     itself (master off -> no FFI call, the per-signal switches
//     ride the request as a probe mask).  TRUST10 heads the section
//     with a divider rule and a bold "Trust" label colored by the
//     verdict level — green on trusted, amber on unknown, red on
//     not-trustable — re-derived on every refresh,
//   - cookie tri-state writing into CookieJar's per-site exception
//     lists, plus a count of the cookies the site has stored,
//   - a "block content on this site" toggle that injects/removes an
//     @@||host^$document whitelist exception in AdBlockManager,
//     flanked (SHLD02) by the global content-blocking switch, a
//     live count of requests blocked on this page and the entry
//     point to the Ad Block settings — the merged successor of the
//     old right-side AdBlockButton popup,
//   - a POPUP01 "allow pop-ups on this site" toggle writing the
//     PopupBlocker per-host exception list,
//   - a SAFE01 "always allow HTTP" toggle writing the HTTPS-Only
//     exception list the warning interstitial consults,
//   - the remembered WebPermissionManager grants for the origin with
//     per-entry revoke buttons (SEC05),
//   - a "clear site data" button wiping the host's cookies and the
//     page's DOM storage (a per-origin scope of SEC12's sweep — the
//     on-disk storage trees still need Chromium's deferred wipe, see
//     BrowserProfile::clearSiteStorage).
//
// Choices persist immediately (the exception lists and the custom
// rules are already QSettings/file backed).
class SitePanel : public QFrame
{
    Q_OBJECT

public:
    explicit SitePanel(QWidget *parent = nullptr);
    void setWebView(WebView *webView);
    WebView *webView() const;

    // SHLD02: requests the blocker stopped on the current page load —
    // the shield button badges its icon with this.  The cumulative
    // interceptor tally is diffed against a baseline captured at
    // load start / url change.
    int blockedSinceLoad() const;

    // TRUST08: the composite verdict's signal lines for a host — the
    // test seam for the Trust section.  Empty when the lookup cannot
    // evaluate (no rustcore, empty host, FFI hiccup).
    static QStringList trustSignalLines(const QString &host, bool isHttps);

    // TRUST09: the verdict's level word ("green"/"amber"/"red") for a
    // host — the shield badge's seam.  Empty when the lookup cannot
    // evaluate.
    static QString trustLevel(const QString &host, bool isHttps);

    // TRUST11: the header's text color for a verdict level, tuned a
    // step darker on light themes and a step lighter on dark ones so
    // it stays readable in both.  Invalid for an unknown level.
    static QColor trustLevelColor(const QString &level, bool darkTheme);

public slots:
    // Repopulates every section for the current page.  Called when the
    // popup opens and after any control writes through.
    void refresh();

signals:
    // Emitted at the end of every refresh — the hosting popup re-fits
    // itself when the content grew while open (TRUST17).
    void refreshed();

private:
    QString host() const;
    QUrl origin() const;
    CookieJar *siteCookieJar() const;
    void applyCookieRule(int index);
    void toggleContentBlocking(bool checked);
    void toggleGlobalBlocking(bool checked);
    int blockedTotal() const;
    void applyJavaScriptRule(int index);
    void togglePopups(bool checked);
    void toggleHttpAllowance(bool checked);
    void toggleFingerprintSpoof(bool checked);
    void clearSiteData();
    void rebuildPermissionRows();
    void rebuildTrustRows();

    // One rendered signal row in the Trust section — alert lines get
    // the palette's warning color.
    struct TrustLine {
        QString text;
        bool alert;
    };
    static QList<TrustLine> trustSignals(const QString &host, bool isHttps);

    QPointer<WebView> m_webView;
    QLabel *m_hostLabel;
    QLabel *m_securityLabel;
    QLabel *m_tlsLabel;
    QWidget *m_trustBox;
    QVBoxLayout *m_trustLayout;
    QLabel *m_trustHeader;
    QComboBox *m_cookieRule;
    QLabel *m_cookieCount;
    QCheckBox *m_contentBlocking;
    QCheckBox *m_blockContent;
    QLabel *m_blockedCount;
    QComboBox *m_javaScriptRule;
    QLabel *m_javaScriptState;
    QCheckBox *m_allowPopups;
    QCheckBox *m_allowHttp;
    QCheckBox *m_spoofFingerprint;
    QWidget *m_permissionsBox;
    QVBoxLayout *m_permissionsLayout;
    QPushButton *m_clearData;
    QPushButton *m_adBlockSettings;
    int m_blockedBaseline;
    bool m_refreshing;
};

#endif // SITEPANEL_H
