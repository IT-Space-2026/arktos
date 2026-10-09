/*
 * Copyright (c) 2026, The Arora Authors
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

#include "omniboxsuggestions.h"

#include "browserapplication.h"
#include "historycompleter.h"
#include "networkaccessmanager.h"
#include "opensearchengine.h"
#include "opensearchmanager.h"
#include "toolbarsearch.h"

#include <qcompleter.h>
#include <qtimer.h>

OmniboxSuggestions::OmniboxSuggestions(OmniboxCompletionModel *model,
                                       QCompleter *completer,
                                       QObject *parent)
    : QObject(parent)
    , m_model(model)
    , m_completer(completer)
    , m_timer(new QTimer(this))
{
    m_timer->setSingleShot(true);
    m_timer->setInterval(200);
    connect(m_timer, &QTimer::timeout,
            this, &OmniboxSuggestions::requestSuggestions);

    OpenSearchManager *manager = ToolbarSearch::openSearchManager();
    connect(manager, &OpenSearchManager::currentEngineChanged,
            this, &OmniboxSuggestions::currentEngineChanged);
    connect(manager, &OpenSearchManager::suggestionsEnabledChanged,
            this, &OmniboxSuggestions::updateSuggestionsEnabled);

    currentEngineChanged();
}

void OmniboxSuggestions::setPrivateContextProvider(
    const std::function<bool()> &provider)
{
    m_privateContextProvider = provider;
    currentEngineChanged();
}

bool OmniboxSuggestions::privateContext() const
{
    return BrowserApplication::isPrivate()
        || (m_privateContextProvider && m_privateContextProvider());
}

bool OmniboxSuggestions::enabled() const
{
    // TOR02: a tor window must never stream keystrokes to a suggest
    // endpoint, regardless of the user's opt-in.
    if (BrowserApplication::isTorMode())
        return false;

    // SRCH04: the address field has its own context switch on top of
    // the per-engine opt-in — the engine-level check happens per
    // request in requestSuggestions().
    return ToolbarSearch::openSearchManager()->suggestionsInAddressField();
}

void OmniboxSuggestions::currentEngineChanged()
{
    // SRCH04/PTAB01: private contexts suggest through the configured
    // private engine, falling back to the default when unset — the
    // context is per-tab now, resolved through the provider.
    m_engine = ToolbarSearch::openSearchManager()
        ->engineForContext(privateContext());
    m_model->setEngineName(m_engine ? m_engine->name() : QString());
    updateSuggestionsEnabled();
}

// SEC11+SRCH04: any permission change — context toggle or per-engine
// opt-in — tears down the pending fetch and the rows already
// delivered.  Which engine the opt-in applies to is resolved per
// request, so the safest response is an unconditional reset; a
// still-allowed pending text simply re-arms the debounce.
void OmniboxSuggestions::updateSuggestionsEnabled()
{
    m_timer->stop();
    if (m_boundEngine)
        m_boundEngine->disconnect(this);
    m_boundEngine = nullptr;
    if (!m_model->suggestions().isEmpty())
        m_model->setSuggestions(QStringList());

    // Toggling an opt-in on mid-edit fetches for the text already
    // typed — the user just consented to it leaving the box.
    if (enabled() && !m_pendingText.trimmed().isEmpty())
        m_timer->start();
}

void OmniboxSuggestions::scheduleSuggestions(const QString &text)
{
    m_pendingText = text;
    if (!enabled())
        return;
    m_timer->start();
}

void OmniboxSuggestions::requestSuggestions()
{
    // Belt-and-suspenders: even a queued timer fires only while the
    // address field is allowed to suggest (SEC11 + SRCH04).
    const QString text = m_pendingText.trimmed();
    if (!enabled() || text.isEmpty())
        return;

    // SRCH06: a shortcut-nickname prefix scopes the dropdown to an
    // app-side provider — the raw scoped text must not reach the
    // suggest endpoint.
    if (ScopeShortcuts::parse(text) != ScopeShortcuts::NoScope)
        return;

    OpenSearchManager *manager = ToolbarSearch::openSearchManager();

    // SRCH04/PTAB01: resolve the engine this input actually targets —
    // re-resolved per request because the private context follows the
    // tab being edited, not the app flag.  In the keyword-only mode
    // anything that does not start with a registered engine keyword
    // stays silent; when it does, the request goes to that keyword's
    // engine instead of the context engine.
    OpenSearchEngine *target = manager->engineForContext(privateContext());
    if (m_engine != target) {
        m_engine = target;
        m_model->setEngineName(target ? target->name() : QString());
    }
    QString query = text;
    const int split = text.indexOf(QLatin1Char(' '));
    OpenSearchEngine *keywordEngine = split > 0
        ? manager->engineForKeyword(text.left(split)) : nullptr;
    if (keywordEngine) {
        // The suggestion goes to the keyword's engine — with the
        // keyword stripped, like convertKeywordSearchToUrl().
        target = keywordEngine;
        query = text.mid(split + 1);
    } else if (manager->suggestionsOnlyWithKeyword()) {
        return;
    }

    // SEC11: the per-engine opt-in still vets every request — a
    // keyword engine that is not opted in suggests nothing either.
    if (!target
        || !target->providesSuggestions()
        || !manager->suggestionsEnabledForEngine(target->name()))
        return;

    if (m_boundEngine != target) {
        if (m_boundEngine)
            m_boundEngine->disconnect(this);
        connect(target, &OpenSearchEngine::suggestions,
                this, &OmniboxSuggestions::newSuggestions,
                Qt::UniqueConnection);
        m_boundEngine = target;
    }

    if (!target->networkAccessManager())
        target->setNetworkAccessManager(
            NetworkAccessManager::instance());
    target->requestSuggestions(query);
}

void OmniboxSuggestions::newSuggestions(const QStringList &suggestions)
{
    // SRCH06: a late reply for pre-scope text must not insert engine
    // rows into a now-scoped view.
    if (!enabled() || !m_model->historyVisible())
        return;

    m_model->setSuggestions(suggestions);

    // The history block may have produced no rows for this prefix, so
    // the popup may not be open yet — nudge the completer like
    // HistoryCompleter::updateFilter() does.
    if (m_completer && m_completer->widget()
        && m_completer->widget()->hasFocus())
        m_completer->complete();
}
