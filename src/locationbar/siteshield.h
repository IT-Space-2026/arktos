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

#include <qpointer.h>
#include <qtoolbutton.h>

class QMenu;
class SitePanel;
class WebView;

// SHLD01: the location-bar shield indicator.  Opens a per-site
// privacy panel (SitePanel, hosted in a QMenu so positioning and
// click-outside dismissal come for free) and reflects the site's
// protection state: normal while content blocking applies, greyed
// when the site is whitelisted or blocking is off globally.
// SHLD02: with the separate AdBlockButton gone this is the single
// shield — it also carries the uBO-style badge counting requests
// blocked on the current page load.
class SiteShieldButton : public QToolButton
{
    Q_OBJECT

public:
    explicit SiteShieldButton(QWidget *parent = nullptr);
    void setWebView(WebView *webView);

    // Test hook — the panel widget inside the button's menu.
    SitePanel *panel() const { return m_panel; }

private:
    void refreshIcon();

    QPointer<WebView> m_webView;
    QMenu *m_menu;
    SitePanel *m_panel;
};

#endif // SITESHIELD_H
