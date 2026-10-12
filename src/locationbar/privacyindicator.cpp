/*
 * Copyright 2009-2010 Benjamin C. Meyer <ben@meyerhome.net>
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

#include "privacyindicator.h"

#include "browserapplication.h"
#include "tabwidget.h"
#include "webview.h"

#include <qicon.h>
#include <qwebenginepage.h>
#include <qwebengineprofile.h>

PrivacyIndicator::PrivacyIndicator(QWidget *parent)
    : QToolButton(parent)
{
    setIcon(QIcon(QLatin1String(":graphics/private.png")));
    setAutoRaise(true);
    setCursor(Qt::ArrowCursor);
    setFocusPolicy(Qt::ClickFocus);
    setToolTip(tr("Private browsing is on. Activate to leave private mode."));
    setAccessibleName(tr("Private Browsing"));
    setAccessibleDescription(
        tr("Private browsing is on. Activate to leave private mode."));
    connect(this, &QToolButton::clicked, this, [this]() {
        if (BrowserApplication::isPrivate()) {
            // Leaving private mode: setPrivate(false) emits
            // privacyChanged and each BrowserMainWindow::privacyChanged
            // clears its tabs — a page's profile cannot be switched in
            // place, so the off-the-record pages have to go.
            BrowserApplication::setPrivate(false);
            return;
        }
        // PTAB01: a private tab inside a normal window — "leave
        // private mode" for it means closing the off-the-record tab.
        if (m_webView && m_webView->tabWidget()) {
            const int index = m_webView->tabWidget()->webViewIndex(m_webView);
            if (index >= 0)
                m_webView->tabWidget()->closeTab(index);
        }
    });
    hide();
}

void PrivacyIndicator::setWebView(WebView *webView)
{
    // Private browsing is a profile property under Qt WebEngine (MIG03):
    // the indicator shows whether this location bar's page lives on an
    // off-the-record profile rather than a global QWebSettings flag.
    m_webView = webView;
    QWebEnginePage *page = webView ? webView->page() : nullptr;
    const bool privatePage = page && page->profile()->isOffTheRecord();
    setVisible(privatePage);
    if (privatePage && !BrowserApplication::isPrivate())
        setToolTip(tr("This is a private tab — nothing it visits is recorded. "
                      "Activate to close it."));
    else
        setToolTip(tr("Private browsing is on. Activate to leave private mode."));
}
