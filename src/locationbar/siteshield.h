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

#ifndef SITESHIELD_H
#define SITESHIELD_H

#include <qicon.h>
#include <qpointer.h>
#include <qtoolbutton.h>

class QMenu;
class SitePanel;
class WebView;
class QWidgetAction;

// SHLD01: the site shield indicator — a single shared button on the
// navigation bar (immediately left of the url field) that follows the
// active tab.  Opens a per-site privacy panel (SitePanel, hosted in a
// QMenu so positioning and click-outside dismissal come for free) and
// reflects the site's protection state: normal while content blocking
// applies, greyed when the site is whitelisted or blocking is off
// globally, disabled on tabs with no web view (TRUST15).
// SHLD02: with the separate AdBlockButton gone this is the single
// shield — it also carries the uBO-style badge counting requests
// blocked on the current page load.
// TRUST09: the same icon doubles as the trust badge — rustcore's
// composite verdict (rc_sitetrust_lookup) tints it amber on an
// "amber" verdict and repaints it red on "red" (threat-list or
// lookalike hit); "green" and un-evaluable pages keep the plain
// shield.  Verdicts are cached per host+https so repaints never wait
// on the lookup, and the cache key carries the scheme so an
// http<->https flip re-evaluates.  TRUST03: the key also carries the
// effective probe mask and trust/enabled short-circuits before the
// cache — a Preferences flip re-evaluates on the next navigation,
// and "off" never reaches the FFI at all.
class SiteShieldButton : public QToolButton
{
    Q_OBJECT

public:
    explicit SiteShieldButton(QWidget *parent = nullptr);
    void setWebView(WebView *webView);

    // Test hook — the panel widget inside the button's menu.
    SitePanel *panel() const { return m_panel; }

    // TRUST09: the verdict level word ("green"/"amber"/"red") the
    // icon currently carries — empty on non-web pages or when the
    // lookup cannot evaluate.  Test seam.
    QString trustBadge() const { return m_trustLevel; }

    // TRUST09: cached verdict level for a host — the first call for
    // a host+https pair runs the (local) FFI lookup, repeats are
    // free.  Test seams.
    static QString trustLevelFor(const QString &host, bool isHttps);
    static int trustLookupCount();
    static void clearTrustCache();

    // TRUST09: the verdict's icon treatment — the base icon back
    // unchanged for anything but "amber"/"red".  Test seam.
    static QIcon tintedBadge(const QIcon &base, const QSize &iconPx,
                             const QString &level);

private:
    void changeEvent(QEvent *event) override;
    void refreshIcon();
    void populateMenu();
    void growMenuToFit();

    QPointer<WebView> m_webView;
    QMenu *m_menu;
    SitePanel *m_panel;
    QString m_trustLevel;
    QWidgetAction *m_panelAction;
};

#endif // SITESHIELD_H
