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

#include "siteshield.h"

#include "adblockmanager.h"
#include "aroraicon.h"
#include "sitepanel.h"
#include "webview.h"

#include <qmenu.h>
#include <qwebenginepage.h>
#include <qwebengineprofile.h>
#include <qwidgetaction.h>

SiteShieldButton::SiteShieldButton(QWidget *parent)
    : QToolButton(parent)
    , m_webView(nullptr)
{
    setAutoRaise(true);
    setCursor(Qt::ArrowCursor);
    setFocusPolicy(Qt::ClickFocus);
    setAccessibleName(tr("Site Privacy"));
    setAccessibleDescription(
        tr("Opens the privacy and permissions panel for this site."));
    setToolTip(tr("Site privacy and permissions"));

    // A menu-hosted widget gets popup positioning, click-outside
    // dismissal and Escape handling for free — a hand-rolled Qt::Popup
    // frame would reimplement all three badly.
    m_menu = new QMenu(this);
    m_panel = new SitePanel(m_menu);
    QWidgetAction *panelAction = new QWidgetAction(m_menu);
    panelAction->setDefaultWidget(m_panel);
    m_menu->addAction(panelAction);
    setMenu(m_menu);
    setPopupMode(QToolButton::InstantPopup);
    connect(m_menu, &QMenu::aboutToShow, m_panel, &SitePanel::refresh);
    connect(AdBlockManager::instance(), &AdBlockManager::rulesChanged,
            this, &SiteShieldButton::refreshIcon);

    refreshIcon();
    setVisible(m_webView != nullptr);
}

void SiteShieldButton::setWebView(WebView *webView)
{
    if (m_webView == webView)
        return;
    if (m_webView)
        disconnect(m_webView, nullptr, this, nullptr);
    m_webView = webView;
    m_panel->setWebView(webView);
    if (webView) {
        connect(webView, &QWebEngineView::urlChanged,
                this, [this](const QUrl &) { refreshIcon(); });
    }
    setVisible(webView != nullptr);
    refreshIcon();
}

void SiteShieldButton::refreshIcon()
{
    const QIcon shield = AroraIcon::get(QLatin1String("security-high"));

    if (!m_webView) {
        setIcon(shield);
        return;
    }

    AdBlockManager *adblock = AdBlockManager::instance();
    const QString host = m_webView->url().host();
    const bool whitelisted = adblock->isSiteWhitelisted(host);
    if (!adblock->isEnabled()) {
        setIcon(QIcon(shield.pixmap(QSize(16, 16), QIcon::Disabled)));
        setToolTip(tr("Site privacy — content blocking is disabled"));
    } else if (whitelisted) {
        setIcon(QIcon(shield.pixmap(QSize(16, 16), QIcon::Disabled)));
        setToolTip(tr("Site privacy — content blocking is off for %1")
                       .arg(host));
    } else {
        setIcon(shield);
        setToolTip(tr("Site privacy and permissions"));
    }
}
