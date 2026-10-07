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
#include "statusbarwidgets.h"

#include "webview.h"
#include "utils/aroraicon.h"

#include <qboxlayout.h>
#include <qlabel.h>
#include <qprogressbar.h>
#include <qtimer.h>
#include <qtoolbutton.h>
#include <qwebengineview.h>

LoadingIndicator::LoadingIndicator(QWidget *parent)
    : QWidget(parent)
    , m_bar(new QProgressBar(this))
    , m_label(new QLabel(this))
    , m_tick(new QTimer(this))
    , m_clearTimer(new QTimer(this))
    , m_loading(false)
    , m_lastElapsedMs(0)
{
    setObjectName(QLatin1String("loadingIndicator"));
    m_bar->setObjectName(QLatin1String("loadingBar"));
    m_label->setObjectName(QLatin1String("loadingLabel"));

    QHBoxLayout *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    m_bar->setRange(0, 100);
    m_bar->setTextVisible(false);
    m_bar->setFixedSize(72, 10);
    layout->addWidget(m_bar);
    layout->addWidget(m_label);

    m_tick->setInterval(250);
    connect(m_tick, &QTimer::timeout,
            this, &LoadingIndicator::updateElapsed);
    m_clearTimer->setSingleShot(true);
    m_clearTimer->setInterval(5000);
    connect(m_clearTimer, &QTimer::timeout,
            this, &LoadingIndicator::clearStatus);

    setVisible(false);
}

WebView *LoadingIndicator::webView() const
{
    return m_view;
}

void LoadingIndicator::setWebView(WebView *view)
{
    if (m_view == view)
        return;
    if (m_view)
        m_view->disconnect(this);
    m_view = view;
    m_loading = false;
    m_tick->stop();
    m_clearTimer->stop();
    setVisible(false);
    if (!m_view)
        return;
    connect(m_view, &QWebEngineView::loadStarted,
            this, &LoadingIndicator::pageLoadStarted);
    connect(m_view, &QWebEngineView::loadProgress,
            this, &LoadingIndicator::pageLoadProgress);
    connect(m_view, &QWebEngineView::loadFinished,
            this, &LoadingIndicator::pageLoadFinished);
    // Attaching mid-load (tab switch during a load) — the elapsed
    // timer can only start from here, so that page under-reports.
    const int progress = m_view->progress();
    if (progress > 0 && progress < 100)
        pageLoadStarted();
}

void LoadingIndicator::pageLoadStarted()
{
    m_loading = true;
    m_lastElapsedMs = 0;
    m_elapsed.start();
    m_clearTimer->stop();
    m_bar->setValue(0);
    m_bar->setVisible(true);
    m_label->setText(QLatin1String("0.0 s"));
    setVisible(true);
    m_tick->start();
}

void LoadingIndicator::pageLoadProgress(int progress)
{
    m_bar->setValue(progress);
    updateElapsed();
}

void LoadingIndicator::pageLoadFinished(bool ok)
{
    m_lastElapsedMs = m_elapsed.isValid() ? m_elapsed.elapsed() : 0;
    m_loading = false;
    m_tick->stop();
    m_bar->setVisible(false);
    const QString elapsed =
        QString::number(m_lastElapsedMs / 1000.0, 'f', 1);
    m_label->setText(ok ? tr("Loaded in %1 s").arg(elapsed)
                        : tr("Load failed after %1 s").arg(elapsed));
    setVisible(true);
    m_clearTimer->start();
}

void LoadingIndicator::updateElapsed()
{
    if (!m_elapsed.isValid())
        return;
    m_label->setText(QString::number(m_elapsed.elapsed() / 1000.0, 'f', 1)
                     + QLatin1String(" s"));
}

void LoadingIndicator::clearStatus()
{
    setVisible(false);
}

ZoomControl::ZoomControl(QWidget *parent)
    : QWidget(parent)
    , m_outButton(new QToolButton(this))
    , m_valueButton(new QToolButton(this))
    , m_inButton(new QToolButton(this))
{
    setObjectName(QLatin1String("zoomControl"));
    m_outButton->setObjectName(QLatin1String("zoomOutButton"));
    m_valueButton->setObjectName(QLatin1String("zoomValueButton"));
    m_inButton->setObjectName(QLatin1String("zoomInButton"));

    QHBoxLayout *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_outButton);
    layout->addWidget(m_valueButton);
    layout->addWidget(m_inButton);

    m_outButton->setIcon(AroraIcon::get(QLatin1String("zoom-out")));
    m_outButton->setAutoRaise(true);
    m_outButton->setIconSize(QSize(14, 14));
    m_outButton->setToolTip(tr("Zoom Out"));
    m_valueButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_valueButton->setAutoRaise(true);
    m_valueButton->setToolTip(tr("Reset Zoom"));
    // Pin the width to the largest ladder entry so the percent does
    // not jitter the status bar on every zoom step.
    m_valueButton->setMinimumWidth(
        fontMetrics().horizontalAdvance(QLatin1String("300%")) + 12);
    m_inButton->setIcon(AroraIcon::get(QLatin1String("zoom-in")));
    m_inButton->setAutoRaise(true);
    m_inButton->setIconSize(QSize(14, 14));
    m_inButton->setToolTip(tr("Zoom In"));

    connect(m_outButton, &QToolButton::clicked, this, [this]() {
        if (m_view)
            m_view->zoomOut();
    });
    connect(m_valueButton, &QToolButton::clicked, this, [this]() {
        if (m_view)
            m_view->resetZoom();
    });
    connect(m_inButton, &QToolButton::clicked, this, [this]() {
        if (m_view)
            m_view->zoomIn();
    });

    setZoom(100);
    setEnabled(false);
}

WebView *ZoomControl::webView() const
{
    return m_view;
}

void ZoomControl::setWebView(WebView *view)
{
    if (m_view == view)
        return;
    if (m_view)
        m_view->disconnect(this);
    m_view = view;
    if (!m_view) {
        setZoom(100);
        setEnabled(false);
        return;
    }
    setEnabled(true);
    connect(m_view, &WebView::zoomChanged,
            this, &ZoomControl::setZoom);
    setZoom(m_view->currentZoom());
}

void ZoomControl::setZoom(int zoom)
{
    m_valueButton->setText(tr("%1%").arg(zoom));
}
