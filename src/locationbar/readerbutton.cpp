/*
 * Copyright 2026 Arktos Browser - IT-Space
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

#include "arktosicon.h"
#include "arktosstyle.h"
#include "browsertheme.h"
#include "readermode.h"
#include "webview.h"

#include <qevent.h>
#include <qfont.h>
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
class ReaderIconEngine : public ScaledPixmapIconEngine
{
public:
    ReaderIconEngine(const QPalette &palette, bool active, bool suggested)
        : m_palette(palette)
        , m_active(active)
        , m_suggested(suggested)
    {
        // TRUST20: bake the Normal-mode art from the Normal group —
        // a refresh while the button sits disabled must not freeze
        // the dim tone into the Normal presentation.
        m_palette.setCurrentColorGroup(QPalette::Normal);
    }

    QIconEngine *clone() const override
    {
        return new ReaderIconEngine(m_palette, m_active, m_suggested);
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
        const QColor fg = mode == QIcon::Disabled
            ? m_palette.color(QPalette::Disabled, QPalette::ButtonText)
            : (m_active ? ArktosStyle::onAccentColor(m_palette)
                        : m_palette.color(m_suggested
                                              ? QPalette::Accent
                                              : QPalette::ButtonText));
        // The "Aa" typeface mark — the reader affordance Safari and
        // Chrome settled on: the document/book silhouettes all fuzzed
        // at the ~12px the disc's inner glyph box leaves, where two
        // letterforms stay crisp at any size.  The painter works in
        // device-independent units (the pixmap's dpr is applied
        // automatically), so the em box is sized off the logical
        // extent — 80% of it puts the cap height at the siblings'
        // optical size.  Suggested keeps the slightly-heavier
        // treatment the glyph stroke used to carry — now just weight.
        const QSizeF logical(pixmap.width() / pixmap.devicePixelRatio(),
                             pixmap.height() / pixmap.devicePixelRatio());
        QFont font = painter.font();
        font.setWeight(m_suggested ? QFont::Bold : QFont::Medium);
        font.setPixelSize(qMax(1, qRound(logical.height() * 0.8)));
        painter.setFont(font);
        painter.setPen(fg);
        painter.setBrush(Qt::NoBrush);
        painter.drawText(QRectF(0, 0, logical.width(), logical.height()),
                         Qt::AlignCenter, QStringLiteral("Aa"));
        painter.end();
        return pixmap;
    }

private:
    QPalette m_palette;
    bool m_active;
    bool m_suggested;
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

void ReaderButton::setAccentOverride(const QColor &color)
{
    if (m_accentOverride == color)
        return;
    m_accentOverride = color;
    refresh();
}

QIcon ReaderButton::makeIcon(bool active, bool suggested) const
{
    // The "Aa" typeface mark, painted so no icon theme is required.
    // The engine paints at whatever size the surface asks for — a
    // single baked pixmap upscaled and blurred next to the
    // theme-crisp siblings (TRUST21).
    // Pastel-circle icon: state-tinted disc (active deeper blue,
    // suggested lighter, plain neutral) with the "Aa" glyph in the
    // disc's contrast ink — the disc carries the state so the glyph
    // itself stays monochrome.
    const QColor disc = active
        ? QColor(QLatin1String("#BBDEFB"))
        : suggested
            ? QColor(QLatin1String("#E3F2FD"))
            : QColor(QLatin1String("#ECEFF1"));
    QPalette palette = this->palette();
    const QColor ink = ArktosStyle::contrastTextColor(disc);
    palette.setColor(QPalette::Text, ink);
    palette.setColor(QPalette::ButtonText, ink);
    palette.setColor(QPalette::Accent, ink);
    palette.setColor(QPalette::Highlight, ink);
    return ArktosIcon::circledIcon(
        QIcon(new ReaderIconEngine(palette, /*active*/ false, suggested)),
        disc);
}

void ReaderButton::refresh()
{
    ReaderMode *mode =
        (m_webView && m_webView->readerMode()) ? m_webView->readerMode()
                                               : nullptr;
    const bool active = mode && mode->isActive();
    const bool suggested = mode && mode->isAvailable() && !active;
    // Presence follows eligibility — Safari/Chrome/Firefox show the
    // reader affordance only on article-like pages.  A permanently
    // visible glyph that refuses on most pages reads as a broken
    // button (the "reader button looks wrong" reports); hiding it
    // until isAvailable() (probed article OR already active) makes
    // appearance itself the affordance.  The toolbar's QWidgetAction
    // mirrors this same gate in updateReaderState() — the widget
    // flag alone latches off under the native NSToolbar (the macOS
    // "still bad" report).  BrowserMainWindow still hides the
    // control outright where it can never apply (widget/engine
    // tabs) — m_webView stays the base gate.
    setVisible(m_webView != nullptr && mode && mode->isAvailable());
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
