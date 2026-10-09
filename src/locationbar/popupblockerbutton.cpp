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

#include "popupblockerbutton.h"

#include "aroraicon.h"
#include "popupblocker.h"
#include "safetext.h"
#include "settings.h"
#include "tabwidget.h"
#include "webpage.h"
#include "webview.h"

#include <qaction.h>
#include <qmenu.h>
#include <qpainter.h>
#include <qwebengineprofile.h>
#include <qwebengineview.h>

PopupBlockerButton::PopupBlockerButton(QWidget *parent)
    : QToolButton(parent)
    , m_webView(nullptr)
    , m_menu(nullptr)
{
    setAutoRaise(true);
    setCursor(Qt::ArrowCursor);
    setFocusPolicy(Qt::ClickFocus);
    setAccessibleName(tr("Blocked Pop-ups"));
    setAccessibleDescription(
        tr("Shows pop-ups this page tried to open, lets you open one "
           "once or always allow pop-ups for the site."));

    // A menu-hosted popup gets positioning, click-outside dismissal
    // and Escape handling for free — same pattern as SiteShieldButton.
    m_menu = new QMenu(this);
    setMenu(m_menu);
    setPopupMode(QToolButton::InstantPopup);
    connect(m_menu, &QMenu::aboutToShow,
            this, &PopupBlockerButton::rebuildMenu);

    setVisible(false);
}

void PopupBlockerButton::setWebView(WebView *webView)
{
    if (m_webView == webView)
        return;
    if (m_webView) {
        disconnect(m_webView, nullptr, this, nullptr);
        if (WebPage *page = webPage())
            disconnect(page, nullptr, this, nullptr);
    }
    m_webView = webView;
    if (webView) {
        connect(webView, &QWebEngineView::urlChanged,
                this, [this](const QUrl &) { refresh(); });
        if (WebPage *page = this->webPage()) {
            connect(page, &WebPage::popupBlocked,
                    this, &PopupBlockerButton::refresh);
        }
    }
    refresh();
}

WebPage *PopupBlockerButton::webPage() const
{
    return m_webView ? m_webView->webPage() : nullptr;
}

void PopupBlockerButton::refresh()
{
    WebPage *page = webPage();
    const int count = page ? page->blockedPopupCount() : 0;
    setVisible(count > 0);
    if (count <= 0)
        return;

    QPixmap pixmap = AroraIcon::get(QLatin1String("window-new"))
        .pixmap(QSize(16, 16));
    // Count badge at the bottom-right corner — same uBO-style
    // treatment the ad-block button uses.
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    const QString text = count > 999
        ? QStringLiteral("…") : QString::number(count);
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
    setIcon(QIcon(pixmap));

    setToolTip(tr("%n pop-up(s) blocked on this page", nullptr, count));
}

void PopupBlockerButton::rebuildMenu()
{
    m_menu->clear();

    WebPage *page = webPage();
    const int count = page ? page->blockedPopupCount() : 0;
    QAction *header = m_menu->addAction(
        tr("%n pop-up(s) blocked on this page", nullptr, count));
    header->setEnabled(false);

    // "Open once" — the target urls the dead-end probes captured.
    // The url text is page-controlled, so it goes through the menu
    // escaper before it can be rendered as markup or a fake shortcut.
    const QList<QUrl> urls = page ? page->blockedPopupUrls()
                                  : QList<QUrl>();
    if (!urls.isEmpty())
        m_menu->addSection(tr("Blocked pop-ups"));
    int shown = 0;
    for (const QUrl &url : urls) {
        if (++shown > 10)
            break;
        QString label = url.toDisplayString();
        if (label.length() > 60)
            label = label.left(57) + QStringLiteral("…");
        QAction *open = m_menu->addAction(
            SafeText::menu(tr("Open %1").arg(label)));
        connect(open, &QAction::triggered,
                this, [this, url]() { openBlockedPopup(url); });
    }

    m_menu->addSeparator();

    const QString host = m_webView ? m_webView->url().host() : QString();
    QAction *allow = m_menu->addAction(
        SafeText::menu(tr("Always allow pop-ups on %1").arg(host)));
    allow->setEnabled(!host.isEmpty());
    connect(allow, &QAction::triggered,
            this, &PopupBlockerButton::allowPopupsForSite);

    QAction *settingsAction = m_menu->addAction(
        tr("Pop-up Settings..."));
    connect(settingsAction, &QAction::triggered, this, [this]() {
        SettingsDialog::openPage(this, SettingsDialog::PrivacyPage);
    });
}

void PopupBlockerButton::openBlockedPopup(const QUrl &url)
{
    WebView *view = m_webView;
    if (!view)
        return;
    // Re-opening through a fresh tab keeps the pop-up out of the
    // page's own window machinery — the tab-placement preference does
    // not apply to a manual "open once" choice.
    if (TabWidget *tabs = view->tabWidget()) {
        tabs->loadUrl(url, TabWidget::NewSelectedTab);
        return;
    }
    WebView *popup = new WebView(view->webPage()->profile());
    popup->setAttribute(Qt::WA_DeleteOnClose);
    popup->loadUrl(url);
    popup->show();
}

void PopupBlockerButton::allowPopupsForSite()
{
    const QString host = m_webView ? m_webView->url().host() : QString();
    if (host.isEmpty())
        return;
    // Off-the-record pages get a session rule — nothing private is
    // ever written to the persistent store.
    const bool persistent =
        !m_webView->webPage()->profile()->isOffTheRecord();
    PopupBlocker::instance()->allowHost(host, persistent);
}
