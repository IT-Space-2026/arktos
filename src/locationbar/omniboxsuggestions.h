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

#ifndef OMNIBOXSUGGESTIONS_H
#define OMNIBOXSUGGESTIONS_H

#include <qobject.h>
#include <qpointer.h>
#include <qstringlist.h>

class QCompleter;
class QTimer;
class OmniboxCompletionModel;
class OpenSearchEngine;

/*
    SRCH01: feeds the location bar's completion dropdown with
    opensearch suggestions, reusing the same data path as
    ToolbarSearch (debounce timer -> OpenSearchEngine::
    requestSuggestions -> suggestions signal -> model rows).

    Keystrokes only ever reach the engine's suggest endpoint while
    the current engine is explicitly opted in via
    OpenSearchManager::setSuggestionsEnabledForEngine() (SEC11) —
    while the flag is off this object issues zero network requests.
    The tor window never suggests at all (TOR02 hardening).
*/
class OmniboxSuggestions : public QObject
{
    Q_OBJECT

public:
    OmniboxSuggestions(OmniboxCompletionModel *model,
                       QCompleter *completer,
                       QObject *parent = nullptr);

public slots:
    // Connect each location bar's QLineEdit::textEdited here.
    void scheduleSuggestions(const QString &text);

private slots:
    void requestSuggestions();
    void newSuggestions(const QStringList &suggestions);
    void currentEngineChanged();
    void updateSuggestionsEnabled();

private:
    bool enabled() const;

    OmniboxCompletionModel *m_model;
    QCompleter *m_completer;
    QTimer *m_timer;
    QString m_pendingText;
    QPointer<OpenSearchEngine> m_engine;
};

#endif // OMNIBOXSUGGESTIONS_H
