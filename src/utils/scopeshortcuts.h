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

#ifndef SCOPESHORTCUTS_H
#define SCOPESHORTCUTS_H

#include <qstring.h>

/*
    SRCH06: Vivaldi-style "shortcut nicknames" for the address field —
    typing a configured token followed by a space at the start of the
    omnibox input scopes the completion dropdown to one app-side
    provider (bookmarks, history or the open tabs) instead of the
    usual history+suggestions merge.

    Each scope has an enabled flag and an editable token, persisted in
    the searchShortcuts QSettings group; both default to the Vivaldi
    spellings (@bookmarks, @history, @tabs) and are on by default.  A
    token that is also a search-engine keyword never scopes — engine
    keyword search keeps first priority, matching
    TabWidget::guessUrlFromString's precedence.
*/
class ScopeShortcuts
{
public:
    enum Scope {
        NoScope = -1,
        BookmarksScope = 0,
        HistoryScope,
        TabsScope
    };

    // Detects "token rest" input.  Returns the matching scope and
    // stores the text after the token+space in rest; NoScope when the
    // leading token is not an enabled configured shortcut (or is an
    // engine keyword — the keyword wins, see class comment).
    static Scope parse(const QString &text, QString *rest = nullptr);

    static bool enabled(Scope scope);
    static void setEnabled(Scope scope, bool enabled);
    static QString token(Scope scope);
    static QString defaultToken(Scope scope);
    static void setToken(Scope scope, const QString &token);

    // Tokens cannot contain whitespace (the first space splits token
    // from search term); the return value is what gets stored.
    static QString sanitizeToken(QString token);

    // Drops every searchShortcuts key back to the compiled defaults.
    static void reset();
};

#endif // SCOPESHORTCUTS_H
