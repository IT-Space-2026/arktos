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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

#ifndef COMMANDPALETTE_H
#define COMMANDPALETTE_H

#include <qframe.h>
#include <qpointer.h>
#include <qset.h>
#include <qstringlist.h>
#include <qurl.h>

class BookmarkNode;
class BrowserMainWindow;
class QAction;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class WebView;

/*!
    CMD01: Ctrl+Shift+P command palette — a VS Code-style fuzzy-search
    overlay over every browser command (menubar actions + window-level
    shortcuts), the open tabs of every browser window, the bookmark
    library and the Settings sections.

    The popup is a Qt::Popup child of the window that opened it, so it
    dismisses itself on focus loss.  The item list is rebuilt on each
    open — tabs and bookmarks are too volatile to cache.  Executing an
    item hides the palette first and runs the payload on the next event
    turn, so command handlers are free to exec() modal dialogs.

    Matching is subsequence-based with word-boundary, consecutive and
    substring bonuses, plus a per-item keyword tail so natural queries
    reach actions whose names don't carry the word (e.g. "clear
    history" -> "Clear Private Data").  Recently executed commands rank
    higher via a small persisted MRU list.
*/
class CommandPalette : public QFrame
{
    Q_OBJECT

public:
    explicit CommandPalette(BrowserMainWindow *window);

    void openPalette();

    // Subsequence score: >= 0 matches (higher is better), -1 no match.
    // Public so tests can pin the ranking rules directly.
    static int fuzzyScore(const QString &query, const QString &candidate);

    // Recently executed item ids, most recent first.
    static QStringList mruIds();

    // Introspection for the smoke run and future tests.
    void setQuery(const QString &query);
    QString query() const;
    int visibleCount() const;
    int currentRow() const;
    QString itemText(int row) const;
    QString itemCategory(int row) const;
    bool executeRow(int row);
    bool executeCurrent();

protected:
    bool eventFilter(QObject *object, QEvent *event) override;

private:
    struct Item {
        QString text;      // what the row shows
        QString category;  // "Command" / "Tab" / "Bookmark" / "Settings"
        QString hint;      // right-aligned: shortcut, url, section
        QString matchText; // text + menu path + keywords — matched, not shown
        QString id;        // stable MRU key (empty: never recorded)
        QIcon icon;
        QPointer<QAction> action;
        QPointer<WebView> view;
        QUrl url;
        int settingsPage = -1;
    };

    void rebuild();
    void collectMenuActions(const QList<QAction*> &actions,
                            const QString &path, QSet<QAction*> *seen);
    void collectCommands();
    void collectTabs();
    void collectBookmarks(BookmarkNode *node);
    void collectSettingsPages();
    void refilter();
    void executeItem(QListWidgetItem *item);
    void executeItem(const Item &item);
    void recordMru(const QString &id);
    void updateGeometry();

    BrowserMainWindow *m_window;
    QLineEdit *m_edit;
    QListWidget *m_list;
    QList<Item> m_items;
};

#endif // COMMANDPALETTE_H
