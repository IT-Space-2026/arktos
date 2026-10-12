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

#include "scopeshortcuts.h"

#include "opensearchmanager.h"
#include "toolbarsearch.h"

#include <qregularexpression.h>
#include <qsettings.h>

static const char *scopeKey(ScopeShortcuts::Scope scope)
{
    switch (scope) {
    case ScopeShortcuts::BookmarksScope: return "bookmarks";
    case ScopeShortcuts::HistoryScope: return "history";
    case ScopeShortcuts::TabsScope: return "tabs";
    default: return nullptr;
    }
}

bool ScopeShortcuts::enabled(Scope scope)
{
    const char *key = scopeKey(scope);
    if (!key)
        return false;
    QSettings settings;
    return settings.value(QLatin1String("searchShortcuts/")
                          + QLatin1String(key), true).toBool();
}

void ScopeShortcuts::setEnabled(Scope scope, bool enabled)
{
    const char *key = scopeKey(scope);
    if (!key)
        return;
    QSettings settings;
    settings.setValue(QLatin1String("searchShortcuts/")
                      + QLatin1String(key), enabled);
}

QString ScopeShortcuts::defaultToken(Scope scope)
{
    switch (scope) {
    case BookmarksScope: return QLatin1String("@bookmarks");
    case HistoryScope: return QLatin1String("@history");
    case TabsScope: return QLatin1String("@tabs");
    default: return QString();
    }
}

QString ScopeShortcuts::token(Scope scope)
{
    const char *key = scopeKey(scope);
    if (!key)
        return QString();
    QSettings settings;
    return settings.value(
        QLatin1String("searchShortcuts/") + QLatin1String(key)
            + QLatin1String("Token"),
        defaultToken(scope)).toString();
}

void ScopeShortcuts::setToken(Scope scope, const QString &token)
{
    const char *key = scopeKey(scope);
    if (!key)
        return;
    QSettings settings;
    settings.setValue(
        QLatin1String("searchShortcuts/") + QLatin1String(key)
            + QLatin1String("Token"),
        token);
}

QString ScopeShortcuts::sanitizeToken(QString token)
{
    return token.remove(QRegularExpression(QLatin1String("\\s+")));
}

ScopeShortcuts::Scope ScopeShortcuts::parse(const QString &text, QString *rest)
{
    if (rest)
        rest->clear();
    const int space = text.indexOf(QLatin1Char(' '));
    if (space <= 0)
        return NoScope;
    const QString first = text.left(space);
    static const Scope scopes[] = { BookmarksScope, HistoryScope, TabsScope };
    for (const Scope scope : scopes) {
        const QString scopeToken = token(scope);
        if (!enabled(scope) || scopeToken.isEmpty() || first != scopeToken)
            continue;
        // Engine keyword search keeps first priority — the same
        // precedence guessUrlFromString() gives
        // convertKeywordSearchToUrl() on Enter.
        if (ToolbarSearch::openSearchManager()->engineForKeyword(first))
            return NoScope;
        if (rest)
            *rest = text.mid(space + 1);
        return scope;
    }
    return NoScope;
}

void ScopeShortcuts::reset()
{
    QSettings settings;
    settings.remove(QLatin1String("searchShortcuts"));
}
