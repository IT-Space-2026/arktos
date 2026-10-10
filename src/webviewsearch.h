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

#ifndef WEBVIEWSEARCH_H
#define WEBVIEWSEARCH_H

#include "searchbar.h"

#include "engineinterface.h"

class WebView;

class WebViewSearch : public SearchBar
{
    Q_OBJECT

public:
    // Takes the widget type rather than WebView so the bar still
    // hosts on a bare engine view (main.cpp's smoke harnesses) — the
    // find paths then no-op (enginePage() only exists on the app's
    // WebView).
    WebViewSearch(QWidget *webView, QWidget *parent = nullptr);

public slots:
    void findNext() override;
    void findPrevious() override;
    void highlightAll();

private:
    void find(Engine::FindFlags flags);
    WebView *webView() const;
};

#include "webview.h"

class WebViewWithSearch : public QWidget
{
    Q_OBJECT

public:
    WebViewWithSearch(WebView *webView, QWidget *parent = nullptr);
    WebView *m_webView;
    WebViewSearch *m_webViewSearch;
};


#endif // WEBVIEWSEARCH_H
