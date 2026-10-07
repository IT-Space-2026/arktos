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

bool OmniboxSuggestions::enabled() const
{
    // TOR02: a tor window must never stream keystrokes to a suggest
    // endpoint, regardless of the user's opt-in.
    if (BrowserApplication::isTorMode())
        return false;

    return m_engine
        && m_engine->providesSuggestions()
        && ToolbarSearch::openSearchManager()
            ->suggestionsEnabledForEngine(m_engine->name());
}

void OmniboxSuggestions::currentEngineChanged()
{
    if (m_engine)
        disconnect(m_engine, &OpenSearchEngine::suggestions,
                   this, &OmniboxSuggestions::newSuggestions);
    m_engine = ToolbarSearch::openSearchManager()->currentEngine();
    m_model->setEngineName(m_engine ? m_engine->name() : QString());
    updateSuggestionsEnabled();
}

// SEC11: the whole data path — the reply hook, the debounce timer and
// the rows themselves — only lives while the current engine is opted
// in.  Disabling drops a pending fetch and any suggestions already
// delivered.
void OmniboxSuggestions::updateSuggestionsEnabled()
{
    if (!enabled()) {
        m_timer->stop();
        if (m_engine)
            disconnect(m_engine, &OpenSearchEngine::suggestions,
                       this, &OmniboxSuggestions::newSuggestions);
        if (!m_model->suggestions().isEmpty())
            m_model->setSuggestions(QStringList());
        return;
    }

    connect(m_engine, &OpenSearchEngine::suggestions,
            this, &OmniboxSuggestions::newSuggestions,
            Qt::UniqueConnection);

    // Toggling the opt-in on mid-edit fetches for the text already
    // typed — the user just consented to it leaving the box.
    if (!m_pendingText.trimmed().isEmpty())
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
    // current engine is opted in (SEC11).
    if (!enabled() || m_pendingText.trimmed().isEmpty())
        return;

    if (!m_engine->networkAccessManager())
        m_engine->setNetworkAccessManager(
            NetworkAccessManager::instance());
    m_engine->requestSuggestions(m_pendingText);
}

void OmniboxSuggestions::newSuggestions(const QStringList &suggestions)
{
    if (!enabled())
        return;

    m_model->setSuggestions(suggestions);

    // The history block may have produced no rows for this prefix, so
    // the popup may not be open yet — nudge the completer like
    // HistoryCompleter::updateFilter() does.
    if (m_completer && m_completer->widget()
        && m_completer->widget()->hasFocus())
        m_completer->complete();
}
