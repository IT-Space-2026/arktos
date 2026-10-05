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

#include "webview.h"

#include <qpixmap.h>
#include <qwebenginepage.h>
#include <qwebengineprofile.h>

PrivacyIndicator::PrivacyIndicator(QWidget *parent)
    : QLabel(parent)
{
    setPixmap(QPixmap(QLatin1String(":graphics/private.png")));
    setCursor(Qt::ArrowCursor);
    hide();
}

void PrivacyIndicator::setWebView(WebView *webView)
{
    // Private browsing is a profile property under Qt WebEngine (MIG03):
    // the indicator shows whether this location bar's page lives on an
    // off-the-record profile rather than a global QWebSettings flag.
    QWebEnginePage *page = webView ? webView->page() : 0;
    setVisible(page && page->profile()->isOffTheRecord());
}

void PrivacyIndicator::mousePressEvent(QMouseEvent *event)
{
    Q_UNUSED(event)
    // TODO(MIG15): leaving private mode means
    // BrowserApplication::setPrivate(false) plus replacing the window's
    // pages — a page's profile cannot be switched in place.
}
