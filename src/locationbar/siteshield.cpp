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
#include "arorastyle.h"
#include "browsertheme.h"
#include "sitepanel.h"
#include "webview.h"

#include <qevent.h>
#include <qmenu.h>
#include <qpainter.h>
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
    // Slightly larger than the stock 16px chrome icons so the shield
    // reads clearly at a glance; still well inside the bar's row.
    setIconSize(QSize(20, 20));
    setAccessibleName(tr("Site Privacy"));
    setAccessibleDescription(
        tr("Opens the privacy and permissions panel for this site."));
    setToolTip(tr("Site privacy and permissions"));

    // A menu-hosted widget gets popup positioning, click-outside
    // dismissal and Escape handling for free — a hand-rolled Qt::Popup
    // frame would reimplement all three badly.
    m_menu = new QMenu(this);
    m_panel = new SitePanel(m_menu);
    m_panelAction = new QWidgetAction(m_menu);
    m_panelAction->setDefaultWidget(m_panel);
    m_menu->addAction(m_panelAction);
    setMenu(m_menu);
    setPopupMode(QToolButton::InstantPopup);
    connect(m_menu, &QMenu::aboutToShow,
            this, &SiteShieldButton::populateMenu);
    // The panel refreshes itself on web-view signals and its own
    // control writes; a refresh that lands while the popup is open can
    // grow the content (TOR16's rule — a shown QMenu never re-fits a
    // hosted widget on its own).
    connect(m_panel, &SitePanel::refreshed,
            this, &SiteShieldButton::growMenuToFit);
    connect(AdBlockManager::instance(), &AdBlockManager::rulesChanged,
            this, &SiteShieldButton::refreshIcon);
    // ICONS05: the icon is baked to a pixmap while the badges are
    // drawn — re-bake on a light<->dark flip so the themed shield art
    // swaps to its -dark variant.  Queued: the signal fires mid-flip,
    // before palette propagation finishes.
    connect(BrowserTheme::themeNotifier(),
            &BrowserTheme::BrowserThemeNotifier::chromeSchemeChanged,
            this, &SiteShieldButton::refreshIcon, Qt::QueuedConnection);

    refreshIcon();
    // TRUST15: never hide the button.  It rides the navigation bar as
    // a QWidgetAction, whose visibility is latched from the widget's
    // state at insertion — a ctor-hidden widget leaves the item
    // permanently absent no matter how many setVisible(true) calls
    // follow (the action's flag governs, not the widget's).
}

void SiteShieldButton::changeEvent(QEvent *event)
{
    // TRUST18: the glyph tint follows the resolved palette — rebake
    // when it moves.  The tor window's violet stylesheet lands on the
    // toolbar AFTER this button is constructed, and palette() only
    // resolves to the chrome's light text once that polish passes.
    if (event->type() == QEvent::PaletteChange
            || event->type() == QEvent::StyleChange)
        refreshIcon();
    QToolButton::changeEvent(event);
}

void SiteShieldButton::populateMenu()
{
    m_panel->refresh();
    // TRUST17 — same InstantPopup size race as the tor circuit popup
    // (TOR19): the tool button resolves the menu's sizeHint() BEFORE
    // aboutToShow to position the popup, which caches the
    // widget-action rect from the panel's previous render and clears
    // the item layout's dirty flag.  QMenu::popup re-runs that pass
    // only when an item changed since, so a refresh() that resized
    // the panel (a permission row appearing, a longer host or count
    // line) still opened inside the stale rect — the first-click
    // squashed popup.  Mutating the action re-dirties the item layout
    // so the post-aboutToShow pass re-measures the fresh content; the
    // panel side of the fix lives in rebuildPermissionRows, which
    // marks its geometry stale so this re-measure reads a real hint.
    // Cheap: one action-rect pass over a single-item menu.
    m_panelAction->setVisible(false);
    m_panelAction->setVisible(true);
}

// Grow only, never shrink — shrinking a popup under the cursor is the
// "creeping popup" class of bug; spare room is harmless.
void SiteShieldButton::growMenuToFit()
{
    if (!m_menu->isVisible())
        return;
    const QSize chrome = m_menu->size() - m_panel->size();
    const QSize grow = (m_panel->sizeHint() + chrome)
        .expandedTo(m_menu->size());
    if (grow != m_menu->size())
        m_menu->resize(grow);
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
        connect(webView, &WebView::javaScriptBlockedChanged,
                this, [this](bool) { refreshIcon(); });
        // Interceptor blocks arrive on the IO thread with no signal —
        // the count badge follows the load's progress/finish beats
        // and the panel recomputes live on popup open.
        connect(webView, &QWebEngineView::loadStarted,
                this, [this]() { refreshIcon(); });
        connect(webView, &QWebEngineView::loadProgress,
                this, [this](int) { refreshIcon(); });
        connect(webView, &QWebEngineView::loadFinished,
                this, [this](bool) { refreshIcon(); });
    }
    refreshIcon();
}

void SiteShieldButton::refreshIcon()
{
    // TRUST18: tint from THIS surface's resolved palette, not the
    // application palette — on the tor window's violet bar the
    // stylesheet lifts ButtonText to the light chrome text color, so
    // the shield stays legible there and on a forced dark theme
    // alike, instead of painting the app's dark glyph.
    const QIcon shield = AroraIcon::getChrome(
        QLatin1String("security-high"), palette());

    // TRUST15: unbound (widget/engine tab, or a window before its
    // first bind) shows the shield disabled with the honest reason —
    // the row keeps its shape instead of gaining a gap.
    if (!m_webView) {
        setIcon(shield);
        setEnabled(false);
        setToolTip(tr("Site privacy — not available on this tab"));
        return;
    }
    setEnabled(true);

    // Badge art is authored in 16x16 space; scale it to the button's
    // icon size so the overlay follows the icon growth.
    const int px = iconSize().width();
    const QSize iconPx(px, px);
    AdBlockManager *adblock = AdBlockManager::instance();
    const QString host = m_webView->url().host();
    const bool whitelisted = adblock->isSiteWhitelisted(host);
    QIcon icon;
    QString tip;
    if (!adblock->isEnabled()) {
        icon = QIcon(shield.pixmap(iconPx, QIcon::Disabled));
        tip = tr("Site privacy — content blocking is disabled");
    } else if (whitelisted) {
        icon = QIcon(shield.pixmap(iconPx, QIcon::Disabled));
        tip = tr("Site privacy — content blocking is off for %1")
                  .arg(host);
    } else {
        icon = shield;
        tip = tr("Site privacy and permissions");
    }

    // SHLD02: the uBO-style blocked-count badge the removed
    // AdBlockButton used to carry, now on the single shield; the
    // panel owns the load-start baseline the count diffs against.
    const int blocked = m_panel->blockedSinceLoad();
    const bool jsBlocked = m_webView->isJavaScriptBlocked();
    if (blocked > 0 || jsBlocked) {
        QPixmap pixmap = icon.pixmap(iconPx);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        const qreal s = qreal(pixmap.width()) / 16.0;
        painter.scale(s, s);
        if (blocked > 0) {
            const QString text = blocked > 999
                ? QStringLiteral("…") : QString::number(blocked);
            QFont font = painter.font();
            font.setPixelSize(8);
            font.setBold(true);
            painter.setFont(font);
            const int w = qMin(15,
                painter.fontMetrics().horizontalAdvance(text) + 4);
            const QRect badge(16 - w, 7, w, 9);
            painter.setPen(Qt::NoPen);
            painter.setBrush(AroraStyle::accentFillColor(palette()));
            painter.drawRoundedRect(badge, 2, 2);
            painter.setPen(AroraStyle::onAccentColor(palette()));
            painter.drawText(badge, Qt::AlignCenter, text);
            tip += tr(" — %1 requests blocked").arg(blocked);
        }
        // JSCTL: badge the shield when the current page's scripts are
        // blocked — a per-site rule, the Safer/Safest tier or the
        // global setting — so the restriction is visible without
        // opening the panel.  It slides to the left corner when the
        // count badge occupies the right one.
        if (jsBlocked) {
            const qreal x = blocked > 0 ? 0.5 : 9.5;
            painter.setBrush(AroraStyle::accentFillColor(palette()));
            painter.setPen(AroraStyle::onAccentColor(palette()));
            painter.drawEllipse(QRectF(x, 9.5, 6.0, 6.0));
            tip += tr(" — JavaScript blocked");
        }
        painter.end();
        icon = QIcon(pixmap);
    }
    setIcon(icon);
    setToolTip(tip);
}
