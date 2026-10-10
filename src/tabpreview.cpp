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

#include "tabpreview.h"

#include "tabbar.h"
#include "tabwidget.h"
#include "webview.h"

#include <qboxlayout.h>
#include <qfontmetrics.h>
#include <qguiapplication.h>
#include <qlabel.h>
#include <qscreen.h>
#include <qtabbar.h>

// Card geometry: a fixed width keeps the popup from jittering while
// the pointer walks the strip; the thumbnail area shrinks to fit the
// captured frame inside it.
static const int CardWidth = 300;
static const int ThumbWidth = CardWidth - 16;
static const int ThumbMaxHeight = 180;
static const int FallbackHeight = 72;

TabPreview::TabPreview(TabBar *bar)
    : QFrame(bar, Qt::ToolTip | Qt::FramelessWindowHint)
    , m_bar(bar)
    , m_thumbnail(new QLabel(this))
    , m_title(new QLabel(this))
    , m_detail(new QLabel(this))
    , m_index(-1)
    , m_hasThumbnail(false)
{
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    setFrameShape(QFrame::StyledPanel);
    setFrameShadow(QFrame::Raised);

    // Page titles/urls are attacker-influenced — plain text only so a
    // '<'-bearing title can never mark the card up.
    m_title->setTextFormat(Qt::PlainText);
    m_detail->setTextFormat(Qt::PlainText);
    QFont titleFont = m_title->font();
    titleFont.setBold(true);
    m_title->setFont(titleFont);
    m_detail->setForegroundRole(QPalette::PlaceholderText);

    m_thumbnail->setAlignment(Qt::AlignCenter);
    m_thumbnail->setFixedWidth(ThumbWidth);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(4);
    layout->addWidget(m_thumbnail, 0, Qt::AlignHCenter);
    layout->addWidget(m_title);
    layout->addWidget(m_detail);
}

int TabPreview::previewedIndex() const
{
    return m_index;
}

QString TabPreview::titleText() const
{
    return m_title->text();
}

QString TabPreview::detailText() const
{
    return m_detail->text();
}

bool TabPreview::hasThumbnail() const
{
    return m_hasThumbnail;
}

void TabPreview::previewTab(int index)
{
    TabWidget *tabWidget = qobject_cast<TabWidget*>(m_bar->parentWidget());
    if (!tabWidget || index < 0 || index >= m_bar->count()) {
        hide();
        return;
    }
    m_index = index;

    WebView *view = tabWidget->webView(index);
    const QString url = view ? view->url().toString() : QString();
    QString title = view ? view->title() : QString();
    if (title.isEmpty())
        title = m_bar->tabText(index);
    if (title.isEmpty())
        title = url.isEmpty() ? tr("New Tab") : url;

    // Sleeping tabs take the favicon+title fallback — their page is
    // discarded so no live frame exists to grab (SLEEP01).
    const bool sleeping = tabWidget->isTabSleeping(index);
    QString detail = url;
    if (sleeping)
        detail = url.isEmpty() ? tr("Sleeping") : tr("Sleeping — %1").arg(url);

    const int textWidth = CardWidth - layout()->contentsMargins().left()
        - layout()->contentsMargins().right();
    m_title->setText(m_title->fontMetrics().elidedText(
        title, Qt::ElideRight, textWidth));
    m_detail->setText(m_detail->fontMetrics().elidedText(
        detail, Qt::ElideMiddle, textWidth));
    m_detail->setVisible(!detail.isEmpty());

    QPixmap thumb;
    if (!sleeping)
        thumb = tabWidget->tabThumbnail(index);
    m_hasThumbnail = !thumb.isNull();
    if (m_hasThumbnail) {
        m_thumbnail->setPixmap(thumb.scaled(ThumbWidth, ThumbMaxHeight,
            Qt::KeepAspectRatio, Qt::SmoothTransformation));
        m_thumbnail->setFixedHeight(m_thumbnail->pixmap().height());
    } else {
        const QIcon icon = m_bar->tabIcon(index);
        m_thumbnail->setPixmap(icon.pixmap(48));
        m_thumbnail->setFixedHeight(FallbackHeight);
    }

    setFixedSize(CardWidth, sizeHint().height());
    positionFor(index);
    show();
    raise();
}

// Anchors the card off the tab's strip-facing edge — below a top bar,
// above a bottom bar, beside a vertical one — then clamps inside the
// screen so edge tabs never lose part of the card off-display.
void TabPreview::positionFor(int index)
{
    const QRect tab = m_bar->tabRect(index);
    QPoint pos;
    switch (m_bar->shape()) {
    case QTabBar::RoundedSouth:
    case QTabBar::TriangularSouth:
        pos = QPoint(tab.left() + (tab.width() - width()) / 2,
                     tab.top() - height() - 6);
        break;
    case QTabBar::RoundedWest:
    case QTabBar::TriangularWest:
        pos = QPoint(tab.right() + 6,
                     tab.top() + (tab.height() - height()) / 2);
        break;
    case QTabBar::RoundedEast:
    case QTabBar::TriangularEast:
        pos = QPoint(tab.left() - width() - 6,
                     tab.top() + (tab.height() - height()) / 2);
        break;
    default:
        pos = QPoint(tab.left() + (tab.width() - width()) / 2,
                     tab.bottom() + 6);
        break;
    }
    // tabRect() is strip-space — when the container-header band is up
    // its height sits between the widget top and the tab row, so the
    // physical point is shifted down by the band.
    pos = m_bar->mapToGlobal(pos + QPoint(0, m_bar->containerStripHeight()));

    QScreen *screen = QGuiApplication::screenAt(pos);
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (screen) {
        const QRect area = screen->availableGeometry();
        pos.setX(qBound(area.left(), pos.x(),
                        area.right() - width() + 1));
        pos.setY(qBound(area.top(), pos.y(),
                        area.bottom() - height() + 1));
    }
    move(pos);
}
