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

#ifndef POPUPBLOCKERBUTTON_H
#define POPUPBLOCKERBUTTON_H

#include <qpointer.h>
#include <qtoolbutton.h>

class QMenu;
class WebPage;
class WebView;

// POPUP01: the location-bar blocked pop-up indicator (Vivaldi/Brave
// style).  Hidden until the current page refuses a pop-up; then it
// appears on the right side near the ad-block button with a count
// badge.  Clicking opens a menu: the per-pop-up "Open once" entries
// the dead-end probes captured, "Always allow pop-ups on <host>"
// (writes the PopupBlocker exception) and a link to the Privacy
// settings page.
class PopupBlockerButton : public QToolButton
{
    Q_OBJECT

public:
    explicit PopupBlockerButton(QWidget *parent = nullptr);
    void setWebView(WebView *webView);

private:
    WebPage *webPage() const;
    void refresh();
    void rebuildMenu();
    void openBlockedPopup(const QUrl &url);
    void allowPopupsForSite();

    QPointer<WebView> m_webView;
    QMenu *m_menu;
};

#endif // POPUPBLOCKERBUTTON_H
