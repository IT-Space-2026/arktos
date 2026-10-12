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
#include "domainblocklist.h"
#include "sitepanel.h"
#include "trancolist.h"
#include "webview.h"

#include <qhash.h>
#include <qmenu.h>
#include <qpainter.h>
#include <qwebenginepage.h>
#include <qwebengineprofile.h>
#include <qwidgetaction.h>

// TRUST09: verdicts cached per host+https, shared across every
// button/tab — one local FFI lookup per combination per session.
// The https bit is part of the key so a scheme flip on the same host
// re-evaluates rather than inheriting the other scheme's verdict.
static QHash<QString, QString> s_trustCache;
static int s_trustLookups = 0;

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
    QWidgetAction *panelAction = new QWidgetAction(m_menu);
    panelAction->setDefaultWidget(m_panel);
    m_menu->addAction(panelAction);
    setMenu(m_menu);
    setPopupMode(QToolButton::InstantPopup);
    connect(m_menu, &QMenu::aboutToShow, m_panel, &SitePanel::refresh);
    connect(AdBlockManager::instance(), &AdBlockManager::rulesChanged,
            this, &SiteShieldButton::refreshIcon);

    // TRUST09: a refreshed blocklist or Tranco table can change the
    // verdict for already-seen hosts — drop the level cache and
    // repaint so the badge follows the new data.
    connect(DomainBlocklist::instance(), &DomainBlocklist::updateFinished,
            this, [this](bool) { clearTrustCache(); refreshIcon(); });
    connect(TrancoList::instance(), &TrancoList::updateFinished,
            this, [this](bool) { clearTrustCache(); refreshIcon(); });

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
    setVisible(webView != nullptr);
    refreshIcon();
}

QString SiteShieldButton::trustLevelFor(const QString &host, bool isHttps)
{
    if (host.isEmpty())
        return QString();
    const QString key = host + QLatin1Char('|')
                        + QLatin1Char(isHttps ? '1' : '0');
    const auto it = s_trustCache.constFind(key);
    if (it != s_trustCache.constEnd())
        return it.value();
    const QString level = SitePanel::trustLevel(host, isHttps);
    ++s_trustLookups;
    s_trustCache.insert(key, level);
    return level;
}

int SiteShieldButton::trustLookupCount()
{
    return s_trustLookups;
}

void SiteShieldButton::clearTrustCache()
{
    s_trustCache.clear();
}

QIcon SiteShieldButton::tintedBadge(const QIcon &base,
                                    const QSize &iconPx,
                                    const QString &level)
{
    QColor tint;
    if (level == QLatin1String("red"))
        // Near-opaque: a threat-list or lookalike verdict overrides
        // the lock outright.
        tint = QColor(214, 48, 49, 195);
    else if (level == QLatin1String("amber"))
        tint = QColor(240, 173, 0, 140);
    else
        return base;
    QPixmap pixmap = base.pixmap(iconPx);
    QPainter painter(&pixmap);
    // SourceAtop keeps the glyph's alpha — a color wash, not a box.
    painter.setCompositionMode(QPainter::CompositionMode_SourceAtop);
    painter.fillRect(pixmap.rect(), tint);
    painter.end();
    return QIcon(pixmap);
}

void SiteShieldButton::refreshIcon()
{
    const QIcon shield = AroraIcon::get(QLatin1String("security-high"));

    m_trustLevel.clear();
    if (!m_webView) {
        setIcon(shield);
        return;
    }

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

    // TRUST09: the composite site-trust verdict marks the lock —
    // red on threat/lookalike hits, an amber wash on an "amber"
    // verdict, untouched on green or when nothing could be
    // evaluated.  The level comes out of the per host+https cache
    // (refreshed by the navigation signals wired in setWebView), so
    // this repaint path never blocks on the lookup; non-web schemes
    // carry no badge at all.
    const QString scheme = m_webView->url().scheme();
    if (scheme == QLatin1String("http")
        || scheme == QLatin1String("https")) {
        m_trustLevel = trustLevelFor(
            host, scheme == QLatin1String("https"));
        icon = tintedBadge(icon, iconPx, m_trustLevel);
        if (m_trustLevel == QLatin1String("red"))
            tip += tr(" — flagged by the phishing/lookalike check");
        else if (m_trustLevel == QLatin1String("amber"))
            tip += tr(" — trust check advises caution");
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
            painter.setBrush(palette().color(QPalette::Accent));
            painter.drawRoundedRect(badge, 2, 2);
            painter.setPen(palette().color(QPalette::HighlightedText));
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
            painter.setBrush(palette().color(QPalette::Accent));
            painter.setPen(palette().color(QPalette::Base));
            painter.drawEllipse(QRectF(x, 9.5, 6.0, 6.0));
            tip += tr(" — JavaScript blocked");
        }
        painter.end();
        icon = QIcon(pixmap);
    }
    setIcon(icon);
    setToolTip(tip);
}
