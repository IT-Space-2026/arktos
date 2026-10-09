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

#include "pipwindow.h"

#include <qapplication.h>
#include <qboxlayout.h>
#include <qcursor.h>
#include <qevent.h>
#include <qfile.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qlabel.h>
#include <qscreen.h>
#include <qsizegrip.h>
#include <qstyle.h>
#include <qtoolbutton.h>
#include <qwebchannel.h>
#include <qwebenginepage.h>
#include <qwebengineview.h>

PipPlayerBridge::PipPlayerBridge(PipWindow *window)
    : QObject(window)
    , m_window(window)
{
}

void PipPlayerBridge::notifyReady()
{
    if (m_window)
        m_window->noteReady();
}

void PipPlayerBridge::notifyError(int code)
{
    if (m_window)
        m_window->noteError(code);
}

void PipPlayerBridge::reportState(double time, bool paused, bool ended,
        double duration)
{
    if (m_window)
        m_window->noteState(time, paused, ended, duration);
}

PipWindow::PipWindow(QWebEngineProfile *profile, const QVariantMap &video,
        const QUrl &baseUrl, QWidget *parent)
    : QWidget(parent, Qt::Window | Qt::FramelessWindowHint
              | Qt::WindowStaysOnTopHint)
    , m_view(new QWebEngineView(this))
    , m_page(new QWebEnginePage(profile, m_view))
    , m_channel(new QWebChannel(this))
    , m_bridge(new PipPlayerBridge(this))
    , m_ready(false)
    , m_error(false)
    , m_closing(false)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setMinimumSize(200, 112);

    // Title strip: drag handle plus the two PiP affordances — return
    // to tab and close.  Standard style icons keep this theme-free.
    QWidget *strip = new QWidget(this);
    strip->setAutoFillBackground(true);
    QPalette stripPalette = strip->palette();
    stripPalette.setColor(QPalette::Window,
            stripPalette.color(QPalette::Window).darker(140));
    strip->setPalette(stripPalette);

    QLabel *title = new QLabel(tr("Picture-in-Picture"), strip);
    title->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    m_returnButton = new QToolButton(strip);
    m_returnButton->setIcon(style()->standardIcon(
            QStyle::SP_TitleBarNormalButton));
    m_returnButton->setToolTip(tr("Return video to tab"));
    m_returnButton->setAutoRaise(true);
    connect(m_returnButton, &QToolButton::clicked,
            this, [this]() { emit returnToTab(m_lastState); });

    QToolButton *closeButton = new QToolButton(strip);
    closeButton->setIcon(style()->standardIcon(
            QStyle::SP_TitleBarCloseButton));
    closeButton->setToolTip(tr("Close"));
    closeButton->setAutoRaise(true);
    connect(closeButton, &QToolButton::clicked,
            this, [this]() { close(); });

    QHBoxLayout *stripLayout = new QHBoxLayout(strip);
    stripLayout->setContentsMargins(8, 2, 4, 2);
    stripLayout->addWidget(title);
    stripLayout->addWidget(m_returnButton);
    stripLayout->addWidget(closeButton);
    stripLayout->addWidget(new QSizeGrip(this), 0,
            Qt::AlignBottom | Qt::AlignRight);

    m_view->setPage(m_page);
    m_channel->registerObject(QLatin1String("aroraPipPlayer"), m_bridge);
    m_page->setWebChannel(m_channel);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(strip);
    layout->addWidget(m_view, 1);
    setLayout(layout);

    // Bottom-right of the screen the cursor is on, like Chrome/Firefox.
    resize(400, 248);
    QScreen *screen = QApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QApplication::primaryScreen();
    if (screen) {
        const QRect available = screen->availableGeometry();
        move(available.right() - width() - 24,
             available.bottom() - height() - 24);
    }

    QString html;
    QFile file(QLatin1String(":pip-player.html"));
    if (file.open(QIODevice::ReadOnly))
        html = QString::fromUtf8(file.readAll());
    const QByteArray configJson = QJsonDocument(
            QJsonObject::fromVariantMap(video))
        .toJson(QJsonDocument::Compact);
    html.replace(QLatin1String("/*__PIP_CONFIG__*/"),
            QLatin1String("window.__PIP_CONFIG__ = ")
                + QString::fromUtf8(configJson) + QLatin1Char(';'));
    m_view->setHtml(html, baseUrl);
}

void PipWindow::setReturnEnabled(bool enabled)
{
    m_returnButton->setEnabled(enabled);
}

void PipWindow::noteReady()
{
    m_ready = true;
    emit ready();
}

void PipWindow::noteState(double time, bool paused, bool ended,
        double duration)
{
    m_lastState[QLatin1String("time")] = time;
    m_lastState[QLatin1String("paused")] = paused;
    m_lastState[QLatin1String("ended")] = ended;
    m_lastState[QLatin1String("duration")] = duration;
}

void PipWindow::noteError(int code)
{
    m_error = true;
    emit failed(tr("The video could not be loaded in "
                   "Picture-in-Picture (error %1).").arg(code));
}

void PipWindow::closeEvent(QCloseEvent *event)
{
    // Both the X button and programmatic closes route the last state
    // out exactly once so the source element can resume in the tab.
    if (!m_closing) {
        m_closing = true;
        emit closing(m_lastState);
    }
    QWidget::closeEvent(event);
}

void PipWindow::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
        m_dragPos = event->globalPosition().toPoint() - frameGeometry().topLeft();
    QWidget::mousePressEvent(event);
}

void PipWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (event->buttons() & Qt::LeftButton)
        move(event->globalPosition().toPoint() - m_dragPos);
    QWidget::mouseMoveEvent(event);
}
