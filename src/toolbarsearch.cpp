/*
 * Copyright 2008 Benjamin C. Meyer <ben@meyerhome.net>
 * Copyright 2009 Jakub Wieczorek <faw217@gmail.com>
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

/****************************************************************************
**
** Copyright (C) 2007-2008 Trolltech ASA. All rights reserved.
**
** This file is part of the demonstration applications of the Qt Toolkit.
**
** This file may be used under the terms of the GNU General Public
** License versions 2.0 or 3.0 as published by the Free Software
** Foundation and appearing in the files LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file.  Alternatively you may (at
** your option) use any later version of the GNU General Public
** License if such license has been publicly approved by Trolltech ASA
** (or its successors, if any) and the KDE Free Qt Foundation. In
** addition, as a special exception, Trolltech gives you certain
** additional rights. These rights are described in the Trolltech GPL
** Exception version 1.2, which can be found at
** http://www.trolltech.com/products/qt/gplexception/ and in the file
** GPL_EXCEPTION.txt in this package.
**
** Please review the following information to ensure GNU General
** Public Licensing requirements will be met:
** http://trolltech.com/products/qt/licenses/licensing/opensource/. If
** you are unsure which license is appropriate for your use, please
** review the following information:
** http://trolltech.com/products/qt/licenses/licensing/licensingoverview
** or contact the sales department at sales@trolltech.com.
**
** In addition, as a special exception, Trolltech, as the sole
** copyright holder for Qt Designer, grants users of the Qt/Eclipse
** Integration plug-in the right for the Qt/Eclipse Integration to
** link to functionality provided by Qt Designer and its related
** libraries.
**
** This file is provided "AS IS" with NO WARRANTY OF ANY KIND,
** INCLUDING THE WARRANTIES OF DESIGN, MERCHANTABILITY AND FITNESS FOR
** A PARTICULAR PURPOSE. Trolltech reserves all rights not expressly
** granted herein.
**
** This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
** WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
**
****************************************************************************/

#include "toolbarsearch.h"

#include "autosaver.h"
#include "networkaccessmanager.h"
#include "opensearchengine.h"
#include "opensearchengineaction.h"
#include "opensearchdialog.h"
#include "opensearchmanager.h"
#include "searchbutton.h"
#include "webpage.h"
#include "webview.h"

#include <qabstractitemview.h>
#include <qcompleter.h>
#include <qcoreapplication.h>
#include <qmenu.h>
#include <qsettings.h>
#include <qstandarditemmodel.h>
#include <qtimer.h>
#include <qurl.h>
#include <qwebengineprofile.h>

OpenSearchManager *ToolbarSearch::s_openSearchManager = 0;

/*
    ToolbarSearch is a search widget that also contains a small history
    and uses open-search for searching.
 */
ToolbarSearch::ToolbarSearch(QWidget *parent)
    : SearchLineEdit(parent)
    , m_suggestionsEnabled(true)
    , m_autosaver(new AutoSaver(this))
    , m_maxSavedSearches(10)
    , m_model(new QStandardItemModel(this))
    , m_suggestionsItem(0)
    , m_recentSearchesItem(0)
    , m_suggestTimer(0)
    , m_completer(0)
{
    connect(openSearchManager(), &OpenSearchManager::currentEngineChanged,
            this, &ToolbarSearch::currentEngineChanged);

    m_completer = new QCompleter(m_model, this);
    m_completer->setCompletionMode(QCompleter::UnfilteredPopupCompletion);
    setCompleter(m_completer);

    searchButton()->setShowMenuTriangle(true);

    connect(searchButton(), &SearchButton::clicked,
            this, &ToolbarSearch::showEnginesMenu);
    connect(this, &QLineEdit::returnPressed,
            this, &ToolbarSearch::searchNow);

    // Qt4 wired these in focusInEvent() because QLineEdit dropped its
    // completer connections on focus-out; Qt6 keeps them on the
    // private control object, so connecting once here is enough (and
    // re-connecting per focus-in would stack duplicates).
    connect(m_completer, QOverload<const QModelIndex &>::of(&QCompleter::activated),
            this, &ToolbarSearch::completerActivated);
    connect(m_completer, QOverload<const QModelIndex &>::of(&QCompleter::highlighted),
            this, &ToolbarSearch::completerHighlighted);

    load();

    currentEngineChanged();
}

OpenSearchManager *ToolbarSearch::openSearchManager()
{
    if (!s_openSearchManager)
        s_openSearchManager = new OpenSearchManager(qApp);
    return s_openSearchManager;
}

void ToolbarSearch::setWebView(WebView *webView)
{
    m_webView = webView;
}

void ToolbarSearch::currentEngineChanged()
{
    OpenSearchEngine *newEngine = openSearchManager()->currentEngine();
    Q_ASSERT(newEngine);
    if (!newEngine)
        return;

    if (m_suggestionsEnabled) {
        if (openSearchManager()->engineExists(m_currentEngine)) {
            OpenSearchEngine *oldEngine = openSearchManager()->engine(m_currentEngine);
            disconnect(oldEngine, &OpenSearchEngine::suggestions,
                       this, &ToolbarSearch::newSuggestions);
        }

        connect(newEngine, &OpenSearchEngine::suggestions,
                this, &ToolbarSearch::newSuggestions);
    }

    setInactiveText(newEngine->name());
    m_currentEngine = newEngine->name();
    m_suggestions.clear();
    setupList();
}

void ToolbarSearch::completerActivated(const QModelIndex &index)
{
    if (completerHighlighted(index))
        searchNow();
}

bool ToolbarSearch::completerHighlighted(const QModelIndex &index)
{
    if (m_suggestionsItem && m_suggestionsItem->index().row() == index.row())
        return false;
    if (m_recentSearchesItem && m_recentSearchesItem->index().row() == index.row())
        return false;
    setText(index.data().toString());
    return true;
}

ToolbarSearch::~ToolbarSearch()
{
    m_autosaver->saveIfNeccessary();
}

void ToolbarSearch::save()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("toolbarsearch"));
    settings.setValue(QLatin1String("recentSearches"), m_recentSearches);
    settings.setValue(QLatin1String("maximumSaved"), m_maxSavedSearches);
    settings.endGroup();
}

void ToolbarSearch::load()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("toolbarsearch"));
    m_recentSearches = settings.value(QLatin1String("recentSearches")).toStringList();
    m_maxSavedSearches = settings.value(QLatin1String("maximumSaved"), m_maxSavedSearches).toInt();

    m_suggestionsEnabled = settings.value(QLatin1String("useSuggestions"), true).toBool();
    if (m_suggestionsEnabled) {
        connect(this, &QLineEdit::textEdited,
                this, &ToolbarSearch::textEdited);
    }

    settings.endGroup();
    setupList();
}

void ToolbarSearch::textEdited(const QString &text)
{
    Q_UNUSED(text);
    // delay creating this to prevent the network manager singleton from
    // being created when it isn't needed on startup
    if (!m_suggestTimer) {
        m_suggestTimer = new QTimer(this);
        m_suggestTimer->setSingleShot(true);
        m_suggestTimer->setInterval(200);
        connect(m_suggestTimer, &QTimer::timeout,
                this, &ToolbarSearch::getSuggestions);
    }
    m_suggestTimer->start();
}

void ToolbarSearch::getSuggestions()
{
    OpenSearchEngine *engine = openSearchManager()->currentEngine();
    Q_ASSERT(engine);
    if (!engine)
        return;

    if (!engine->networkAccessManager())
        engine->setNetworkAccessManager(NetworkAccessManager::instance());

    engine->requestSuggestions(text());
}

void ToolbarSearch::searchNow()
{
    OpenSearchEngine *engine = openSearchManager()->currentEngine();
    Q_ASSERT(engine);
    if (!engine)
        return;

    QString searchText = text();

    // Private browsing is a profile property under Qt WebEngine (MIG03):
    // recent searches are only recorded when the bound view's page is
    // not on an off-the-record profile.
    QWebEnginePage *page = m_webView ? m_webView->webPage() : 0;
    if (!page || !page->profile()->isOffTheRecord()) {
        QStringList newList = m_recentSearches;
        if (newList.contains(searchText))
            newList.removeAt(newList.indexOf(searchText));
        newList.prepend(searchText);
        if (newList.size() >= m_maxSavedSearches)
            newList.removeLast();

        m_recentSearches = newList;
        m_autosaver->changeOccurred();
    }

    QUrl searchUrl = engine->searchUrl(searchText);
    TabWidget::OpenUrlIn tab = TabWidget::CurrentTab;
    if (qApp->keyboardModifiers() == Qt::AltModifier)
        tab = TabWidget::NewSelectedTab;
    emit search(searchUrl, tab);
}

void ToolbarSearch::newSuggestions(const QStringList &suggestions)
{
    m_suggestions = suggestions;
    setupList();
}

void ToolbarSearch::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::LanguageChange)
        retranslate();
    SearchLineEdit::changeEvent(event);
}

void ToolbarSearch::retranslate()
{
    if (m_suggestionsItem)
        m_suggestionsItem->setText(tr("Suggestions"));
}

void ToolbarSearch::showEnginesMenu()
{
    QMenu menu;

    QWidget *parent = searchButton()->parentWidget();
    if (!parent)
        return;

    QPoint pos = parent->mapToGlobal(QPoint(0, parent->height()));

    QList<QString> list = openSearchManager()->allEnginesNames();
    for (int i = 0; i < list.count(); ++i) {
        QString name = list.at(i);
        OpenSearchEngine *engine = openSearchManager()->engine(name);
        OpenSearchEngineAction *action = new OpenSearchEngineAction(engine, &menu);
        action->setData(name);
        connect(action, &QAction::triggered, this, &ToolbarSearch::changeCurrentEngine);
        menu.addAction(action);

        if (openSearchManager()->currentEngineName() == name) {
            action->setCheckable(true);
            action->setChecked(true);
        }
    }

    // Page-advertised engines go between these two separators.
    QAction *enginesSeparator = menu.addSeparator();

    // TODO(MIG14): use the BrowserMainWindow's searchManagerAction()
    // once it exists again so the entry also lives in the Tools menu.
    menu.addAction(tr("Configure Search Engines..."), this, &ToolbarSearch::showEnginesDialog);

    if (!m_recentSearches.isEmpty())
        menu.addAction(tr("Clear Recent Searches"), this, &ToolbarSearch::clear);

    // WebEngine has no synchronous DOM access; the page's linked
    // resources are collected in the render process and arrive while
    // this menu's exec() runs its nested event loop, so the "Add"
    // actions appear as soon as the page reports them.
    if (m_webView) {
        WebPage *page = m_webView->webPage();
        QPointer<QMenu> menuGuard(&menu);
        QPointer<WebView> viewGuard(m_webView);
        page->linkedResources(QStringLiteral("search"),
                [this, menuGuard, viewGuard, enginesSeparator](const QList<WebPageLinkedResource> &engines) {
            if (!menuGuard)
                return;
            for (const WebPageLinkedResource &engine : engines) {
                QUrl url = engine.href;
                QString title = engine.title;

                if (engine.type != QLatin1String("application/opensearchdescription+xml"))
                    continue;
                if (url.isEmpty())
                    continue;

                if (title.isEmpty())
                    title = viewGuard && !viewGuard->title().isEmpty()
                            ? viewGuard->title() : url.host();

                QAction *action = new QAction(tr("Add '%1'").arg(title), menuGuard);
                connect(action, &QAction::triggered, this, &ToolbarSearch::addEngineFromUrl);
                action->setData(url);
                if (viewGuard)
                    action->setIcon(viewGuard->icon());
                menuGuard->insertAction(enginesSeparator, action);
            }
        });
    }

    menu.exec(pos);
}

void ToolbarSearch::showEnginesDialog()
{
    OpenSearchDialog dialog(this);
    dialog.exec();
}

void ToolbarSearch::changeCurrentEngine()
{
    if (QAction *action = qobject_cast<QAction*>(sender())) {
        QString name = action->data().toString();
        openSearchManager()->setCurrentEngineName(name);
    }
}

void ToolbarSearch::addEngineFromUrl()
{
    QAction *action = qobject_cast<QAction*>(sender());
    if (!action)
        return;
    QVariant variant = action->data();
    if (!variant.canConvert<QUrl>())
        return;
    QUrl url = variant.toUrl();

    openSearchManager()->addEngine(url);
}

void ToolbarSearch::setupList()
{
    if (m_suggestions.isEmpty()
        || (m_model->rowCount() > 0
            && m_model->item(0) != m_suggestionsItem)) {
        m_model->clear();
        m_suggestionsItem = 0;
    } else {
        m_model->removeRows(1, m_model->rowCount() - 1);
    }

    QFont lightFont;
    lightFont.setWeight(QFont::Light);
    if (!m_suggestions.isEmpty()) {
        if (m_model->rowCount() == 0) {
            if (!m_suggestionsItem) {
                m_suggestionsItem = new QStandardItem();
                m_suggestionsItem->setFont(lightFont);
                retranslate();
            }
            m_model->appendRow(m_suggestionsItem);
        }
        for (int i = 0; i < m_suggestions.count(); ++i) {
            const QString &text = m_suggestions.at(i);
            m_model->appendRow(new QStandardItem(text));
        }
    }

    if (m_recentSearches.isEmpty()) {
        m_recentSearchesItem = new QStandardItem(tr("No Recent Searches"));
        m_recentSearchesItem->setFont(lightFont);
        m_model->appendRow(m_recentSearchesItem);
    } else {
        m_recentSearchesItem = new QStandardItem(tr("Recent Searches"));
        m_recentSearchesItem->setFont(lightFont);
        m_model->appendRow(m_recentSearchesItem);
        for (int i = 0; i < m_recentSearches.count(); ++i) {
            QString text = m_recentSearches.at(i);
            m_model->appendRow(new QStandardItem(text));
        }
    }

    QAbstractItemView *view = completer()->popup();
    view->setFixedHeight(view->sizeHintForRow(0) * m_model->rowCount() + view->frameWidth() * 2);
}

void ToolbarSearch::clear()
{
    m_recentSearches.clear();
    m_autosaver->changeOccurred();
    setupList();
    QLineEdit::clear();
    clearFocus();
}

