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

#ifndef ADBLOCKBUTTON_H
#define ADBLOCKBUTTON_H

#include <qpointer.h>
#include <qtoolbutton.h>

class QMenu;
class QAction;
class WebView;

// ADB05: the location-bar content-blocker indicator (uBO/Vivaldi
// style).  Clicking opens a small popup: per-page blocked count,
// a global enable/disable toggle and an entry that opens the
// AdBlock configuration dialog.  The icon is the bundled
// security-high shield tinted toward blocker red so it does not
// read as a second copy of the site-shield button; it greys out
// while content blocking is disabled or the site is whitelisted,
// and carries a numeric badge of requests blocked on the page.
class AdBlockButton : public QToolButton
{
    Q_OBJECT

public:
    explicit AdBlockButton(QWidget *parent = nullptr);
    void setWebView(WebView *webView);

private:
    void refresh();
    void refreshMenu();
    // Cumulative count for the page's first-party host, and the
    // portion attributed to the current page load (diffed against a
    // baseline captured at load start).
    int blockedTotal() const;
    int blockedSinceLoad() const;

    QPointer<WebView> m_webView;
    QMenu *m_menu;
    QAction *m_countAction;
    QAction *m_enabledAction;
    int m_baseline;
};

#endif // ADBLOCKBUTTON_H
