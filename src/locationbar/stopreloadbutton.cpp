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

#include "stopreloadbutton.h"

#include "aroraicon.h"
#include "engineinterface.h"
#include "webview.h"

StopReloadButton::StopReloadButton(QWidget *parent)
    : QToolButton(parent)
    , m_webView(nullptr)
    , m_loading(false)
{
    setObjectName(QLatin1String("stopReloadButton"));
    setAutoRaise(true);
    setCursor(Qt::ArrowCursor);
    setFocusPolicy(Qt::ClickFocus);
    setIconSize(QSize(16, 16));
    connect(this, &QToolButton::clicked, this, [this]() {
        if (!m_webView || !m_webView->enginePage())
            return;
        if (m_loading)
            m_webView->enginePage()->stop();
        else
            m_webView->enginePage()->reload();
    });
    setLoading(false);
}

void StopReloadButton::setWebView(WebView *webView)
{
    m_webView = webView;
    // Mid-load attach — the next loadStarted may have already gone.
    setLoading(webView && webView->isLoading());
}

void StopReloadButton::setLoading(bool loading)
{
    m_loading = loading;
    if (loading) {
        setIcon(AroraIcon::get(QStringLiteral("process-stop")));
        setToolTip(tr("Stop loading this page"));
        setAccessibleName(tr("Stop"));
    } else {
        setIcon(AroraIcon::get(QStringLiteral("view-refresh")));
        setToolTip(tr("Reload this page"));
        setAccessibleName(tr("Reload"));
    }
}
