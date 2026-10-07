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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

#include "adblockbutton.h"

#include "adblockmanager.h"
#include "adblockrequestinterceptor.h"
#include "aroraicon.h"
#include "webview.h"

#include <qaction.h>
#include <qmenu.h>
#include <qpainter.h>
#include <qwebenginepage.h>

AdBlockButton::AdBlockButton(QWidget *parent)
    : QToolButton(parent)
    , m_webView(nullptr)
    , m_menu(nullptr)
    , m_countAction(nullptr)
    , m_enabledAction(nullptr)
    , m_baseline(0)
{
    setAutoRaise(true);
    setCursor(Qt::ArrowCursor);
    setFocusPolicy(Qt::ClickFocus);
    setAccessibleName(tr("Content Blocker"));
    setAccessibleDescription(
        tr("Shows blocked-request count and opens the ad-blocker "
           "settings."));

    // A menu-hosted popup gets positioning, click-outside dismissal
    // and Escape handling for free — same pattern as SiteShieldButton.
    m_menu = new QMenu(this);
    m_countAction = m_menu->addAction(QString());
    m_countAction->setEnabled(false);
    m_menu->addSeparator();
    m_enabledAction = m_menu->addAction(tr("Content blocking"));
    m_enabledAction->setCheckable(true);
    connect(m_enabledAction, &QAction::triggered,
            this, [](bool checked) {
        AdBlockManager::instance()->setEnabled(checked);
    });
    QAction *configure = m_menu->addAction(tr("Ad Block Settings..."));
    connect(configure, &QAction::triggered,
            this, []() { AdBlockManager::instance()->showDialog(); });
    setMenu(m_menu);
    setPopupMode(QToolButton::InstantPopup);
    connect(m_menu, &QMenu::aboutToShow,
            this, &AdBlockButton::refreshMenu);
    connect(AdBlockManager::instance(), &AdBlockManager::rulesChanged,
            this, [this]() { refresh(); });

    refresh();
    setVisible(m_webView != nullptr);
}

void AdBlockButton::setWebView(WebView *webView)
{
    if (m_webView == webView)
        return;
    if (m_webView)
        disconnect(m_webView, nullptr, this, nullptr);
    m_webView = webView;
    if (webView) {
        // A new load (or a same-tab navigation to another host) starts
        // a fresh tally: the counter is cumulative, so the baseline is
        // re-captured rather than the shared table cleared.
        connect(webView, &QWebEngineView::loadStarted,
                this, [this]() {
            m_baseline = blockedTotal();
            refresh();
        });
        connect(webView, &QWebEngineView::urlChanged,
                this, [this](const QUrl &) {
            m_baseline = blockedTotal();
            refresh();
        });
        // Interceptor blocks arrive on the IO thread with no signal —
        // the badge follows the load's progress/finish beats and the
        // popup recomputes live on open.
        connect(webView, &QWebEngineView::loadProgress,
                this, [this](int) { refresh(); });
        connect(webView, &QWebEngineView::loadFinished,
                this, [this](bool) { refresh(); });
        m_baseline = blockedTotal();
    }
    setVisible(webView != nullptr);
    refresh();
}

int AdBlockButton::blockedTotal() const
{
    if (!m_webView)
        return 0;
    const QString host = m_webView->url().host();
    if (host.isEmpty())
        return 0;
    return AdBlockRequestInterceptor::blockedRequestCount(host);
}

int AdBlockButton::blockedSinceLoad() const
{
    return qMax(0, blockedTotal() - m_baseline);
}

void AdBlockButton::refresh()
{
    AdBlockManager *adblock = AdBlockManager::instance();
    const bool enabled = adblock->isEnabled();
    const QString host = m_webView ? m_webView->url().host() : QString();
    const bool whitelisted = enabled && !host.isEmpty()
        && adblock->isSiteWhitelisted(host);
    const int blocked = blockedSinceLoad();

    const QIcon base = AroraIcon::get(QLatin1String("security-high"));
    QPixmap pixmap;
    if (!enabled || whitelisted) {
        pixmap = base.pixmap(QSize(16, 16), QIcon::Disabled);
    } else {
        // The only shield glyph in the bundled sets is flat mid-grey —
        // SourceIn tinting recolors it to a blocker red so the button
        // is not mistaken for the site-privacy shield.
        pixmap = base.pixmap(QSize(16, 16));
        QPainter painter(&pixmap);
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(pixmap.rect(), QColor(0xb0, 0x45, 0x3c));
        painter.end();
    }

    if (blocked > 0) {
        // uBO-style count badge at the bottom-right corner.
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
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
        painter.end();
    }
    setIcon(QIcon(pixmap));

    QString tip;
    if (!enabled)
        tip = tr("Content blocking is disabled");
    else if (whitelisted)
        tip = tr("Content blocking is off for %1").arg(host);
    else if (blocked > 0)
        tip = tr("Content blocker — %1 requests blocked on this page")
                  .arg(blocked);
    else
        tip = tr("Content blocker");
    setToolTip(tip);
}

void AdBlockButton::refreshMenu()
{
    const bool enabled = AdBlockManager::instance()->isEnabled();
    const QString host = m_webView ? m_webView->url().host() : QString();
    if (!enabled) {
        m_countAction->setText(tr("Content blocking is disabled"));
    } else if (host.isEmpty()) {
        m_countAction->setText(tr("No web page to count"));
    } else {
        m_countAction->setText(
            tr("%1 requests blocked on this page")
                .arg(blockedSinceLoad()));
    }
    m_enabledAction->setChecked(enabled);
}
