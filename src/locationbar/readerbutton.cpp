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

#include "readerbutton.h"

#include "browsertheme.h"
#include "readermode.h"
#include "webview.h"

#include <qpainter.h>

ReaderButton::ReaderButton(QWidget *parent)
    : QToolButton(parent)
    , m_webView(nullptr)
{
    setAutoRaise(true);
    setCursor(Qt::ArrowCursor);
    setFocusPolicy(Qt::ClickFocus);
    setCheckable(true);
    setAccessibleName(tr("Reader Mode"));
    setAccessibleDescription(
        tr("Shows the page in a clutter-free reader view."));
    connect(this, &QToolButton::clicked, this, [this]() {
        if (m_webView)
            m_webView->readerMode()->toggle();
        // Auto-checkable flips the check before this slot runs; a
        // refused enter() emits no activeChanged, so re-assert.
        refresh();
    });
    // ICONS05: the glyph is painted from the palette at refresh()
    // time — repaint it when a light<->dark flip lands.  Queued: the
    // signal fires mid-flip, before palette propagation finishes.
    connect(BrowserTheme::themeNotifier(),
            &BrowserTheme::BrowserThemeNotifier::chromeSchemeChanged,
            this, [this]() { refresh(); }, Qt::QueuedConnection);
    hide();
}

void ReaderButton::setWebView(WebView *webView)
{
    if (m_webView == webView)
        return;
    if (m_webView && m_webView->readerMode())
        disconnect(m_webView->readerMode(), nullptr, this, nullptr);
    m_webView = webView;
    if (webView && webView->readerMode()) {
        connect(webView->readerMode(), &ReaderMode::availableChanged,
                this, [this](bool) { refresh(); });
        connect(webView->readerMode(), &ReaderMode::activeChanged,
                this, [this](bool) { refresh(); });
    }
    refresh();
}

QIcon ReaderButton::makeIcon(bool active, bool suggested) const
{
    // A page-with-lines glyph, painted so no icon theme is required.
    // Active state fills the page with the accent color; suggested
    // pages get an accent outline + dot so availability still hints
    // at reader mode without gating the button itself.
    QPixmap pixmap(16, 16);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor fg = palette().color(
        active ? QPalette::HighlightedText
               : suggested ? QPalette::Accent : QPalette::ButtonText);
    const QRectF page(2.5, 1.5, 11, 13);
    if (active) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(palette().color(QPalette::Accent));
        painter.drawRoundedRect(page, 1.5, 1.5);
    }
    painter.setPen(QPen(fg, suggested ? 1.4 : 1.1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(page, 1.5, 1.5);
    painter.drawLine(QPointF(4.5, 5), QPointF(11.5, 5));
    painter.drawLine(QPointF(4.5, 8), QPointF(11.5, 8));
    painter.drawLine(QPointF(4.5, 11), QPointF(11.5, 11));
    if (suggested) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(palette().color(QPalette::Accent));
        painter.drawEllipse(QPointF(12.6, 3.0), 1.8, 1.8);
    }
    painter.end();
    return QIcon(pixmap);
}

void ReaderButton::refresh()
{
    ReaderMode *mode =
        (m_webView && m_webView->readerMode()) ? m_webView->readerMode()
                                               : nullptr;
    const bool active = mode && mode->isActive();
    const bool suggested = mode && mode->isAvailable() && !active;
    // READ04: presence follows the tab — a WebView always offers
    // reader mode (BrowserMainWindow hides the control where it can
    // never apply: widget/engine tabs); the heuristic verdict only
    // changes the "suggested" affordance.
    setVisible(m_webView != nullptr);
    setChecked(active);
    setProperty("suggested", suggested);
    setIcon(makeIcon(active, suggested));
    setToolTip(active
        ? tr("Exit reader view (Ctrl+Alt+R)")
        : suggested
            ? tr("Enter reader view — page looks like an article "
                 "(Ctrl+Alt+R)")
            : tr("Enter reader view (Ctrl+Alt+R)"));
}
