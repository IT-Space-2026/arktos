/*
 * Copyright 2008 Benjamin C. Meyer <ben@meyerhome.net>
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

#ifndef LOCATIONBAR_H
#define LOCATIONBAR_H

#include "lineedit.h"

#include <qpointer.h>
#include <qurl.h>

class WebView;
class LocationBarSiteIcon;
class PrivacyIndicator;
class SiteShieldButton;
class AdBlockButton;
class LocationBar : public LineEdit
{
    Q_OBJECT

public:
    LocationBar(QWidget *parent = nullptr);
    void setWebView(WebView *webView);
    WebView *webView() const;

protected:
    void paintEvent(QPaintEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private slots:
    void webViewUrlChanged(const QUrl &url);

private:
    QPointer<WebView> m_webView;

    LocationBarSiteIcon *m_siteIcon;
    SiteShieldButton *m_shield;
    AdBlockButton *m_adBlockButton;
    PrivacyIndicator *m_privacyIndicator;
};

#endif // LOCATIONBAR_H

