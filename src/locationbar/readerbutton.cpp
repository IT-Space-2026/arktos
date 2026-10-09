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
    });
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

QIcon ReaderButton::makeIcon(bool active) const
{
    // A page-with-lines glyph, painted so no icon theme is required.
    // Active state fills the page with the accent color.
    QPixmap pixmap(16, 16);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor fg = palette().color(
        active ? QPalette::HighlightedText : QPalette::ButtonText);
    const QRectF page(2.5, 1.5, 11, 13);
    if (active) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(palette().color(QPalette::Accent));
        painter.drawRoundedRect(page, 1.5, 1.5);
    }
    painter.setPen(QPen(fg, 1.1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(page, 1.5, 1.5);
    painter.drawLine(QPointF(4.5, 5), QPointF(11.5, 5));
    painter.drawLine(QPointF(4.5, 8), QPointF(11.5, 8));
    painter.drawLine(QPointF(4.5, 11), QPointF(11.5, 11));
    painter.end();
    return QIcon(pixmap);
}

void ReaderButton::refresh()
{
    ReaderMode *mode =
        (m_webView && m_webView->readerMode()) ? m_webView->readerMode()
                                               : nullptr;
    const bool available = mode && mode->isAvailable();
    const bool active = mode && mode->isActive();
    setVisible(available);
    setChecked(active);
    setIcon(makeIcon(active));
    setToolTip(active
        ? tr("Exit reader view (Ctrl+Alt+R)")
        : tr("Enter reader view (Ctrl+Alt+R)"));
}
