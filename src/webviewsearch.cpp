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

#include <qwebenginefindtextresult.h>
#include <qwebengineview.h>

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
}

void WebViewSearch::findNext()
{
    // Qt WebEngine's find always wraps around the document; the
    // WebKit FindWrapsAroundDocument flag is gone.
    find(QWebEnginePage::FindFlags());
}

void WebViewSearch::findPrevious()
{
    find(QWebEnginePage::FindBackward);
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
        find(QWebEnginePage::FindFlags());
    else
        webView()->findText(QString());
}

void WebViewSearch::find(QWebEnginePage::FindFlags flags)
{
    QString searchString = ui.searchLineEdit->text();
    if (!webView() || searchString.isEmpty())
        return;
    // findText answers asynchronously from the render process.
    QPointer<WebViewSearch> guard(this);
    webView()->findText(searchString, flags,
                        [guard](const QWebEngineFindTextResult &result) {
        if (guard) {
            guard->ui.searchInfo->setText(
                result.numberOfMatches() > 0 ? QString()
                                             : WebViewSearch::tr("Not Found"));
        }
    });
}

QWebEngineView *WebViewSearch::webView() const
{
    return qobject_cast<QWebEngineView*>(searchObject());
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
