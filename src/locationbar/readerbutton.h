/*
 * Copyright 2026 Arktos Browser - IT-Space
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
// single reader control (READ03 removed the in-bar twin).  Presence
// follows ReaderMode::isAvailable() — the Safari/Chrome/Firefox
// contract where the affordance only exists on article-eligible
// pages (a permanently visible glyph that refuses on most pages
// reads as a broken button); the toolbar's QWidgetAction mirrors the
// same gate since native toolbars ignore the widget's hidden flag.
// Checked while the reader overlay is up.  Click toggles reader mode.
class ReaderButton : public QToolButton
{
    Q_OBJECT

public:
    ReaderButton(QWidget *parent = nullptr);
    void setWebView(WebView *webView);

    // THEM06: an accent override applied to the icon's palette
    // snapshot at bake time — for chrome whose accent differs from
    // the app palette's.  The tor bar needs this because a stylesheet
    // repolish rebuilds the widget palette from QApplication::palette
    // + the sheet's roles, wiping a widget-level Accent seed (the
    // sheet only rewrites ButtonText).  An invalid color clears it.
    void setAccentOverride(const QColor &color);
    QColor accentOverride() const { return m_accentOverride; }

private:
    void changeEvent(QEvent *event) override;
    void refresh();
    QIcon makeIcon(bool active, bool suggested) const;

    QPointer<WebView> m_webView;
    QColor m_accentOverride;
};

#endif // READERBUTTON_H
