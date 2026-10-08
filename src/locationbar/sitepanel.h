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

#include <qframe.h>
#include <qpointer.h>
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
//   - cookie tri-state writing into CookieJar's per-site exception
//     lists, plus a count of the cookies the site has stored,
//   - a "block content on this site" toggle that injects/removes an
//     @@||host^$document whitelist exception in AdBlockManager,
//   - a POPUP01 "allow pop-ups on this site" toggle writing the
//     PopupBlocker per-host exception list,
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

public slots:
    // Repopulates every section for the current page.  Called when the
    // popup opens and after any control writes through.
    void refresh();

private:
    QString host() const;
    QUrl origin() const;
    CookieJar *siteCookieJar() const;
    void applyCookieRule(int index);
    void toggleContentBlocking(bool checked);
    void applyJavaScriptRule(int index);
    void togglePopups(bool checked);
    void clearSiteData();
    void rebuildPermissionRows();

    QPointer<WebView> m_webView;
    QLabel *m_hostLabel;
    QLabel *m_securityLabel;
    QComboBox *m_cookieRule;
    QLabel *m_cookieCount;
    QCheckBox *m_blockContent;
    QComboBox *m_javaScriptRule;
    QLabel *m_javaScriptState;
    QCheckBox *m_allowPopups;
    QWidget *m_permissionsBox;
    QVBoxLayout *m_permissionsLayout;
    QPushButton *m_clearData;
    bool m_refreshing;
};

#endif // SITEPANEL_H
