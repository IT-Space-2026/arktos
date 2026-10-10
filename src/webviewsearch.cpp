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

#include "webviewsearch.h"

#include <qevent.h>
#include <qlayout.h>
#include <qlineedit.h>
#include <qpointer.h>
#include <qshortcut.h>
#include <qtimeline.h>
#include <qtoolbutton.h>

#include <qdebug.h>

WebViewSearch::WebViewSearch(QWebEngineView *webView, QWidget *parent)
    : SearchBar(parent)
{
    setSearchObject(webView);
    ui.highlightAllButton->setVisible(true);
    connect(ui.highlightAllButton, &QToolButton::toggled,
            this, &WebViewSearch::highlightAll);
    connect(ui.searchLineEdit, &QLineEdit::textEdited,
            this, &WebViewSearch::highlightAll);
    // findText answers asynchronously from the render process; the
    // result carries the total match count and the active ordinal
    // (verified 1-based on Qt 6.12 — it displays as-is).  One page
    // feeds exactly one search bar, so the broadcast result slot can
    // own the label outright.
    if (WebView *view = this->webView()) {
        connect(view->enginePage(), &Engine::Page::findTextFinished,
                this, [this](const Engine::FindResult &result) {
            // Clearing finds (empty needle) leave the label blank —
            // the counter only narrates a real query.
            if (ui.searchLineEdit->text().isEmpty())
                return;
            if (result.numberOfMatches > 0) {
                ui.searchInfo->setText(WebViewSearch::tr("%1/%2")
                    .arg(result.activeMatch)
                    .arg(result.numberOfMatches));
            } else {
                ui.searchInfo->setText(WebViewSearch::tr("Not Found"));
            }
        });
    }
}

void WebViewSearch::findNext()
{
    // Qt WebEngine's find always wraps around the document; the
    // WebKit FindWrapsAroundDocument flag is gone.
    find(Engine::FindFlags());
}

void WebViewSearch::findPrevious()
{
    find(Engine::FindBackward);
}

void WebViewSearch::highlightAll()
{
    if (!webView())
        return;
    // WebEngine has no HighlightAllOccurrences flag: a find always
    // highlights every match in the renderer.  The toggle can only
    // choose between showing those highlights (run the find again)
    // and clearing them (findText with an empty string); the next
    // findNext()/findPrevious() re-highlights either way.
    if (ui.highlightAllButton->isChecked())
        find(Engine::FindFlags());
    else {
        webView()->enginePage()->findText(QString(), Engine::FindFlags());
        ui.searchInfo->setText(QString());
    }
}

void WebViewSearch::find(Engine::FindFlags flags)
{
    QString searchString = ui.searchLineEdit->text();
    if (!webView())
        return;
    // An emptied field clears both the renderer's highlights and the
    // counter — leaving them stale would claim matches that are gone.
    if (searchString.isEmpty()) {
        webView()->enginePage()->findText(QString(), Engine::FindFlags());
        ui.searchInfo->setText(QString());
        return;
    }
    webView()->enginePage()->findText(searchString, flags);
}

WebView *WebViewSearch::webView() const
{
    return qobject_cast<WebView*>(searchObject());
}

WebViewWithSearch::WebViewWithSearch(WebView *webView, QWidget *parent)
    : QWidget(parent)
    , m_webView(webView)
{
    m_webView->setParent(this);
    QVBoxLayout *layout = new QVBoxLayout;
    layout->setSpacing(0);
    layout->setContentsMargins(0, 0, 0, 0);
    m_webViewSearch = new WebViewSearch(m_webView, this);
    layout->addWidget(m_webViewSearch);
    layout->addWidget(m_webView);
    setLayout(layout);
}
