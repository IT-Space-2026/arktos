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

#ifndef READERBUTTON_H
#define READERBUTTON_H

#include <qpointer.h>
#include <qtoolbutton.h>

class WebView;

// READ02: the navigation-toolbar reader-mode button — the browser's
// single reader control (READ03 removed the in-bar twin).  READ04:
// always present on engine tabs — the article heuristic is allowed to
// be wrong, so it no longer gates presence; ReaderMode::isAvailable()
// only drives the "suggested" accent.  Checked while the reader
// overlay is up.  Click toggles reader mode.
class ReaderButton : public QToolButton
{
    Q_OBJECT

public:
    ReaderButton(QWidget *parent = nullptr);
    void setWebView(WebView *webView);

private:
    void changeEvent(QEvent *event) override;
    void refresh();
    QIcon makeIcon(bool active, bool suggested) const;

    QPointer<WebView> m_webView;
};

#endif // READERBUTTON_H
