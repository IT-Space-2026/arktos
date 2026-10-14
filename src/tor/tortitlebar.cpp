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

#include "tortitlebar.h"

#include "arorastyle.h"

#include <qapplication.h>
#include <qboxlayout.h>
#include <qevent.h>
#include <qfontmetrics.h>
#include <qlabel.h>
#include <qmenu.h>
#include <qmenubar.h>
#include <qpainter.h>
#include <qtoolbutton.h>
#include <qwindow.h>

// TOR12: the drawn window title on the violet chrome.  A SqueezeLabel
// elides by rewriting its own text(), which shrinks the sizeHint the
// menubar's corner-widget negotiation reads — one narrow frame can
// therefore collapse the corner permanently, and the full title is
// lost until the next WindowTitleChange.  This label keeps the real
// title in m_fullTitle, reports the FULL title width as its sizeHint
// (so the corner keeps its room and the menu items are what move),
// and only swaps the elided form into text() when the assigned width
// cannot hold the title — the elide adapts both directions and never
// destroys the string.
class TorTitleLabel : public QLabel
{
public:
    explicit TorTitleLabel(QWidget *parent = nullptr)
        : QLabel(parent)
    {
    }

    void setTitleText(const QString &title)
    {
        m_fullTitle = title;
        setToolTip(title);
        syncShownText();
        updateGeometry();
    }

    QString titleText() const { return m_fullTitle; }

    QSize sizeHint() const override
    {
        QSize hint = QLabel::sizeHint();
        hint.setWidth(fontMetrics().horizontalAdvance(m_fullTitle)
                      + contentsMargins().left()
                      + contentsMargins().right());
        return hint;
    }

protected:
    void changeEvent(QEvent *event) override
    {
        QLabel::changeEvent(event);
        if (event->type() == QEvent::FontChange
                || event->type() == QEvent::StyleChange)
            syncShownText();
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QLabel::resizeEvent(event);
        syncShownText();
    }

private:
    void syncShownText()
    {
        QString shown = m_fullTitle;
        const int room = contentsRect().width();
        if (room > 0) {
            const QFontMetrics metrics(fontMetrics());
            if (metrics.horizontalAdvance(shown) > room)
                shown = metrics.elidedText(shown, Qt::ElideMiddle, room);
        }
        if (text() != shown)
            QLabel::setText(shown);
    }

    QString m_fullTitle;
};

namespace {

enum TitleGlyph { GlyphMinimize, GlyphMaximize, GlyphRestore, GlyphClose };

// The window controls are painted, not themed — the bundled icon sets
// ship no window-* art and a themed pixmap cannot be relied on to
// read correctly on the violet bar.  Light glyphs in the chrome's
// text color like the rest of the tor chrome text.
QIcon titleGlyphIcon(TitleGlyph glyph, const QWidget *widget)
{
    const int extent = 16;
    const qreal dpr = widget->devicePixelRatioF();
    QPixmap pixmap(QSize(extent, extent) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    QPen pen(AroraStyle::torTextColor());
    pen.setWidthF(1.3);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    switch (glyph) {
    case GlyphMinimize:
        painter.drawLine(QPointF(4.0, 12.0), QPointF(12.0, 12.0));
        break;
    case GlyphMaximize:
        painter.drawRect(QRectF(4.25, 4.25, 7.5, 7.5));
        break;
    case GlyphRestore:
        painter.drawLine(QPointF(6.0, 3.5), QPointF(12.5, 3.5));
        painter.drawLine(QPointF(12.5, 3.5), QPointF(12.5, 10.0));
        painter.fillRect(QRectF(3.5, 6.0, 7.5, 7.5),
                         AroraStyle::torChromeColor());
        painter.drawRect(QRectF(3.5, 6.0, 7.5, 7.5));
        break;
    case GlyphClose:
        painter.drawLine(QPointF(4.25, 4.25), QPointF(11.75, 11.75));
        painter.drawLine(QPointF(11.75, 4.25), QPointF(4.25, 11.75));
        break;
    }
    return QIcon(pixmap);
}

} // namespace

TorTitleBar::TorTitleBar(QWidget *window)
    : QWidget(window)
    , m_window(window)
    , m_menuBar(window->findChild<QMenuBar *>())
    , m_controls(new QWidget(window))
    , m_titleLabel(new TorTitleLabel(this))
    , m_minimizeButton(new QToolButton(m_controls))
    , m_maximizeButton(new QToolButton(m_controls))
    , m_closeButton(new QToolButton(m_controls))
{
    // This widget is the menubar's left corner: Tor badge + live
    // window title, and a drag surface.
    setObjectName(QLatin1String("torTitleBar"));
    setFixedHeight(30);

    QLabel *badge = new QLabel(tr("Tor"), this);
    badge->setObjectName(QLatin1String("torTitleBadge"));
    badge->setToolTip(tr("This window browses through the Tor network"));

    m_titleLabel->setObjectName(QLatin1String("torTitleText"));
    m_titleLabel->setTitleText(m_window->windowTitle());
    // The cap bounds the corner so a very long title cannot push the
    // menus into the overflow extension; the label itself elides.
    m_titleLabel->setMaximumWidth(420);

    QHBoxLayout *layout = new QHBoxLayout(this);
    layout->setContentsMargins(AroraStyle::Spacing::Medium, 0,
                               AroraStyle::Spacing::Medium, 0);
    layout->setSpacing(AroraStyle::Spacing::Medium);
    layout->addWidget(badge);
    layout->addWidget(m_titleLabel);

    // The violet headline/badge styling lives in the central style
    // source (THEME01) — palette-quoted tor family only.
    setStyleSheet(AroraStyle::torTitleBarStyleSheet());

    // The right corner carries the window controls — painted glyphs in
    // the same violet hover family as the rest of the tor chrome.
    m_controls->setObjectName(QLatin1String("torTitleControls"));
    m_controls->setFixedHeight(30);
    struct ButtonSpec {
        QToolButton *button;
        TitleGlyph glyph;
        const char *name;
    };
    const ButtonSpec specs[] = {
        { m_minimizeButton, GlyphMinimize, "torTitleMinimize" },
        { m_maximizeButton, GlyphMaximize, "torTitleMaximize" },
        { m_closeButton, GlyphClose, "torTitleClose" },
    };
    for (const ButtonSpec &spec : specs) {
        spec.button->setObjectName(QLatin1String(spec.name));
        spec.button->setAutoRaise(true);
        spec.button->setFixedSize(30, 26);
        spec.button->setIconSize(QSize(16, 16));
        spec.button->setIcon(titleGlyphIcon(spec.glyph, this));
        spec.button->setCursor(Qt::ArrowCursor);
    }
    m_minimizeButton->setToolTip(tr("Minimize"));
    m_minimizeButton->setAccessibleName(tr("Minimize window"));
    m_closeButton->setToolTip(tr("Close"));
    m_closeButton->setAccessibleName(tr("Close window"));

    connect(m_minimizeButton, &QToolButton::clicked,
            m_window, &QWidget::showMinimized);
    connect(m_maximizeButton, &QToolButton::clicked,
            this, &TorTitleBar::toggleMaximized);
    connect(m_closeButton, &QToolButton::clicked,
            m_window, &QWidget::close);

    QHBoxLayout *controlsLayout = new QHBoxLayout(m_controls);
    controlsLayout->setContentsMargins(0, 0,
                                       AroraStyle::Spacing::Medium, 0);
    controlsLayout->setSpacing(AroraStyle::Spacing::Small);
    controlsLayout->addWidget(m_minimizeButton);
    controlsLayout->addWidget(m_maximizeButton);
    controlsLayout->addWidget(m_closeButton);
    m_controls->setStyleSheet(
        AroraStyle::torTitleControlsStyleSheet());

    if (m_menuBar) {
        m_menuBar->setCornerWidget(this, Qt::TopLeftCorner);
        m_menuBar->setCornerWidget(m_controls, Qt::TopRightCorner);
        m_menuBar->installEventFilter(this);
    }

    // The frame ring around the content is the window's own
    // background — a violet border that doubles as the edge-resize
    // grab zone.  Scoped by objectName so nothing else picks the rule
    // up.
    m_window->setObjectName(QLatin1String("torMainWindow"));
    m_window->setStyleSheet(AroraStyle::torWindowStyleSheet());
    m_window->setMouseTracking(true);
    m_window->installEventFilter(this);
    syncFrameMargins();
    syncWindowButtons();
}

void TorTitleBar::syncCornerWidth()
{
    // The menubar renegotiates the corner-widget width only when its
    // items are dirty — a changed title sizeHint does not reach that
    // pass, so without this nudge the corner keeps the previous
    // title's width and a longer title stays squeezed until some
    // unrelated menubar relayout.  A synthetic resize runs the same
    // dirty + updateGeometries pass a menu change does: our new
    // sizeHint claims its room and the menu items are what move.
    if (!m_menuBar)
        return;
    QResizeEvent relayout(m_menuBar->size(), m_menuBar->size());
    QApplication::sendEvent(m_menuBar, &relayout);
}

Qt::Edges TorTitleBar::frameEdges(const QRect &windowRect,
                                  const QPoint &pos)
{
    Qt::Edges edges;
    if (pos.x() < windowRect.left() + FrameMargin)
        edges |= Qt::LeftEdge;
    if (pos.x() > windowRect.right() - FrameMargin)
        edges |= Qt::RightEdge;
    if (pos.y() < windowRect.top() + FrameMargin)
        edges |= Qt::TopEdge;
    if (pos.y() > windowRect.bottom() - FrameMargin)
        edges |= Qt::BottomEdge;
    return edges;
}

bool TorTitleBar::eventFilter(QObject *object, QEvent *event)
{
    if (object == m_menuBar) {
        // Empty menubar space is the title-bar drag surface; menu
        // items keep their normal behavior.
        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            QMouseEvent *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() == Qt::LeftButton
                    && !m_menuBar->actionAt(mouseEvent->pos())) {
                beginDrag(mouseEvent->globalPosition().toPoint());
                return true;
            }
            break;
        }
        case QEvent::MouseButtonDblClick: {
            QMouseEvent *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() == Qt::LeftButton
                    && !m_menuBar->actionAt(mouseEvent->pos())) {
                toggleMaximized();
                return true;
            }
            break;
        }
        case QEvent::MouseMove: {
            QMouseEvent *mouseEvent = static_cast<QMouseEvent *>(event);
            if ((mouseEvent->buttons() & Qt::LeftButton)
                    && !m_dragOffset.isNull()) {
                m_window->move(mouseEvent->globalPosition().toPoint()
                               - m_dragOffset);
                return true;
            }
            break;
        }
        case QEvent::MouseButtonRelease: {
            QMouseEvent *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() == Qt::LeftButton
                    && !m_dragOffset.isNull()) {
                endDrag();
                return true;
            }
            break;
        }
        case QEvent::ContextMenu:
            showWindowMenu(static_cast<QContextMenuEvent *>(event)
                               ->globalPos());
            return true;
        default:
            break;
        }
        return QWidget::eventFilter(object, event);
    }

    if (object == m_window) {
        switch (event->type()) {
        case QEvent::WindowTitleChange:
            m_titleLabel->setTitleText(m_window->windowTitle());
            syncCornerWidth();
            break;
        case QEvent::WindowStateChange:
            syncWindowButtons();
            syncFrameMargins();
            break;
        case QEvent::Leave:
            m_window->unsetCursor();
            break;
        case QEvent::MouseMove: {
            QMouseEvent *mouseEvent = static_cast<QMouseEvent *>(event);
            syncFrameCursor(mouseEvent->position().toPoint());
            break;
        }
        case QEvent::MouseButtonPress: {
            QMouseEvent *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() == Qt::LeftButton && resizable()) {
                const Qt::Edges edges = frameEdges(m_window->rect(),
                        mouseEvent->position().toPoint());
                if (edges && m_window->windowHandle()
                        && m_window->windowHandle()
                               ->startSystemResize(edges))
                    return true;
            }
            break;
        }
        default:
            break;
        }
    }
    return QWidget::eventFilter(object, event);
}

void TorTitleBar::beginDrag(const QPoint &globalPos)
{
    if (QWindow *handle = m_window->windowHandle()) {
        if (handle->startSystemMove())
            return;
    }
    // No system move on this platform (e.g. offscreen) — drag the
    // window ourselves like PipWindow does.
    if (!m_window->isMaximized())
        m_dragOffset = globalPos - m_window->frameGeometry().topLeft();
}

void TorTitleBar::endDrag()
{
    m_dragOffset = QPoint();
}

void TorTitleBar::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        beginDrag(event->globalPosition().toPoint());
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void TorTitleBar::mouseMoveEvent(QMouseEvent *event)
{
    if ((event->buttons() & Qt::LeftButton) && !m_dragOffset.isNull())
        m_window->move(event->globalPosition().toPoint() - m_dragOffset);
    QWidget::mouseMoveEvent(event);
}

void TorTitleBar::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
        endDrag();
    QWidget::mouseReleaseEvent(event);
}

void TorTitleBar::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        toggleMaximized();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void TorTitleBar::contextMenuEvent(QContextMenuEvent *event)
{
    showWindowMenu(event->globalPos());
}

void TorTitleBar::showWindowMenu(const QPoint &globalPos)
{
    QMenu menu(this);
    if (m_window->isMaximized())
        menu.addAction(tr("Restore"), m_window, &QWidget::showNormal);
    else
        menu.addAction(tr("Maximize"), m_window, &QWidget::showMaximized);
    menu.addAction(tr("Minimize"), m_window, &QWidget::showMinimized);
    menu.addSeparator();
    menu.addAction(tr("Close"), m_window, &QWidget::close);
    menu.exec(globalPos);
}

void TorTitleBar::toggleMaximized()
{
    if (m_window->isMaximized())
        m_window->showNormal();
    else
        m_window->showMaximized();
}

bool TorTitleBar::resizable() const
{
    return m_window->windowHandle()
        && !m_window->isMaximized()
        && !m_window->isFullScreen()
        && !m_window->isMinimized();
}

void TorTitleBar::syncWindowButtons()
{
    const bool maximized = m_window->isMaximized();
    m_maximizeButton->setIcon(titleGlyphIcon(
            maximized ? GlyphRestore : GlyphMaximize, this));
    m_maximizeButton->setToolTip(
            maximized ? tr("Restore") : tr("Maximize"));
    m_maximizeButton->setAccessibleName(
            maximized ? tr("Restore window") : tr("Maximize window"));
}

void TorTitleBar::syncFrameMargins()
{
    const int margin = (m_window->isMaximized() || m_window->isFullScreen())
        ? 0 : FrameMargin;
    m_window->setContentsMargins(margin, margin, margin, margin);
}

void TorTitleBar::syncFrameCursor(const QPoint &pos)
{
    if (!resizable()) {
        m_window->unsetCursor();
        return;
    }
    const Qt::Edges edges = frameEdges(m_window->rect(), pos);
    if (!edges) {
        m_window->unsetCursor();
        return;
    }
    const bool horizontal =
        edges.testFlag(Qt::LeftEdge) || edges.testFlag(Qt::RightEdge);
    const bool vertical =
        edges.testFlag(Qt::TopEdge) || edges.testFlag(Qt::BottomEdge);
    Qt::CursorShape shape = Qt::ArrowCursor;
    if (horizontal && vertical) {
        const bool falling =
            (edges.testFlag(Qt::TopEdge) && edges.testFlag(Qt::LeftEdge))
            || (edges.testFlag(Qt::BottomEdge)
                && edges.testFlag(Qt::RightEdge));
        shape = falling ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor;
    } else if (horizontal) {
        shape = Qt::SizeHorCursor;
    } else {
        shape = Qt::SizeVerCursor;
    }
    m_window->setCursor(shape);
}
