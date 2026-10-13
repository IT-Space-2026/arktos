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

#ifndef STOPRELOADBUTTON_H
#define STOPRELOADBUTTON_H

#include <qpointer.h>
#include <qtoolbutton.h>

class WebView;

// NAVB01: the combined stop/reload glyph at the right end of the
// location bar — the same face rule as the toolbar's stopReload
// action (Stop while the page loads, Reload otherwise), routed to
// the page's own Stop/Reload action on click.  Kept in step by
// LocationBar via setLoading() on loadStarted/loadFinished because
// Qt 6.12 emits no mid-flight loadProgress values to key on.
class StopReloadButton : public QToolButton
{
    Q_OBJECT

public:
    StopReloadButton(QWidget *parent = nullptr);
    void setWebView(WebView *webView);
    void setLoading(bool loading);
    bool loading() const { return m_loading; }

private:
    QPointer<WebView> m_webView;
    bool m_loading;
};

#endif // STOPRELOADBUTTON_H
