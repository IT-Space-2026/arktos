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
class ReaderButton;
class SiteShieldButton;
class AdBlockButton;
class PopupBlockerButton;
class LocationBar : public LineEdit
{
    Q_OBJECT

public:
    LocationBar(QWidget *parent = nullptr);
    void setWebView(WebView *webView);
    WebView *webView() const;

    // SAFE03: anti-phishing display aids.
    //
    // registrableDomainRange() returns the character range of the
    // registrable domain (eTLD+1 approximation) inside an encoded url
    // string so the bar can de-emphasize everything around it.  Qt6
    // dropped QUrl::topLevelDomain + the public-suffix list, so
    // multi-label registry boundaries (co.uk, com.au, ...) ride the
    // cookie jar's two-level ccTLD table; anything else assumes a
    // one-label suffix and hosts with too few labels emphasize whole.
    //
    // unicodeUrlHint() returns the decoded (Unicode) url when the
    // displayed host carries an internationalized name that is shown
    // in punycode, an empty string otherwise.
    static bool registrableDomainRange(const QString &displayText,
                                       int &start, int &length);
    static QString unicodeUrlHint(const QString &displayText);

protected:
    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private slots:
    void webViewUrlChanged(const QUrl &url);

private:
    void displayUrl(const QUrl &url);

    QPointer<WebView> m_webView;

    LocationBarSiteIcon *m_siteIcon;
    SiteShieldButton *m_shield;
    AdBlockButton *m_adBlockButton;
    PopupBlockerButton *m_popupBlockerButton;
    PrivacyIndicator *m_privacyIndicator;
    ReaderButton *m_readerButton;
};

#endif // LOCATIONBAR_H

