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

#include "arorastyle.h"
#include "browsertheme.h"
#include "readermode.h"
#include "webview.h"

#include <qevent.h>
#include <qiconengine.h>
#include <qpainter.h>

namespace {

// TRUST21: the reader glyph rides the same contract ChromeIconEngine
// gives the themed nav buttons — painted at the size the bar asks
// for, tinted from a palette snapshot, per-mode colors.  The old
// path baked one fixed 16px pixmap: the toolbar's 18px request
// painted it upscaled and blurred — a visibly grey glyph next to the
// crisp siblings, most obvious on a dark palette — and its Disabled
// presentation came from Qt's generated ghosting (near-white on a
// white glyph) instead of the palette's dimmed tone.  refresh()
// rebakes the snapshot whenever the palette moves (changeEvent), so
// the glyph still follows light<->dark flips and the tor seed.
class ReaderIconEngine : public QIconEngine
{
public:
    ReaderIconEngine(const QPalette &palette, bool active)
        : m_palette(palette)
        , m_active(active)
    {
        // TRUST20: bake the Normal-mode art from the Normal group —
        // a refresh while the button sits disabled must not freeze
        // the dim tone into the Normal presentation.
        m_palette.setCurrentColorGroup(QPalette::Normal);
    }

    QIconEngine *clone() const override
    {
        return new ReaderIconEngine(m_palette, m_active);
    }

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode,
               QIcon::State state) override
    {
        const qreal scale = painter->device()
            ? painter->device()->devicePixelRatioF() : 1.0;
        painter->drawPixmap(rect.topLeft(),
                            scaledPixmap(rect.size(), mode, state, scale));
    }

    QPixmap pixmap(const QSize &size, QIcon::Mode mode,
                   QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1.0);
    }

    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode,
                         QIcon::State, qreal scale) override
    {
        const QSize pixels = size * scale;
        QPixmap pixmap(pixels.isEmpty() ? QSize(16, 16) : pixels);
        pixmap.setDevicePixelRatio(scale);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        // The glyph is drawn on a 16x16 grid, scaled uniformly to the
        // requested size and centered — page outline + three lines.
        const qreal k = qMin(pixmap.width(), pixmap.height()) / 16.0;
        painter.translate((pixmap.width() - 16 * k) / 2.0,
                          (pixmap.height() - 16 * k) / 2.0);
        painter.scale(k, k);
        const QColor fg = mode == QIcon::Disabled
            ? m_palette.color(QPalette::Disabled, QPalette::ButtonText)
            : (m_active ? AroraStyle::onAccentColor(m_palette)
                        : m_palette.color(QPalette::ButtonText));
        const QRectF page(2.5, 1.5, 11, 13);
        if (m_active) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(AroraStyle::accentFillColor(m_palette));
            painter.drawRoundedRect(page, 1.5, 1.5);
        }
        painter.setPen(QPen(fg, 1.1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(page, 1.5, 1.5);
        painter.drawLine(QPointF(4.5, 5), QPointF(11.5, 5));
        painter.drawLine(QPointF(4.5, 8), QPointF(11.5, 8));
        painter.drawLine(QPointF(4.5, 11), QPointF(11.5, 11));
        painter.end();
        return pixmap;
    }

private:
    QPalette m_palette;
    bool m_active;
};

} // namespace

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

void ReaderButton::changeEvent(QEvent *event)
{
    // TRUST20: the glyph is baked from palette() at refresh() time,
    // and nothing rebaked it afterwards — the same stale-icon class
    // TRUST18 fixed on the shield.  A stylesheet (un)polish restores
    // then re-applies the widget palette, so a refresh() landing
    // inside that transient (or any palette move between refreshes)
    // pinned a black glyph on the tor bar until the next state
    // change.  Rebake whenever the palette/style settles — the last
    // event of a cascade always carries the resolved color.
    if (event->type() == QEvent::PaletteChange
            || event->type() == QEvent::StyleChange
            || event->type() == QEvent::EnabledChange)
        refresh();
    QToolButton::changeEvent(event);
}

QIcon ReaderButton::makeIcon(bool active) const
{
    // A page-with-lines glyph, painted so no icon theme is required.
    // Active state fills the page with the accent color.  The engine
    // paints at whatever size the surface asks for — a single baked
    // pixmap upscaled and blurred next to the theme-crisp siblings
    // (TRUST21).
    return QIcon(new ReaderIconEngine(palette(), active));
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
