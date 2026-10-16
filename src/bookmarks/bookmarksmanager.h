/*
 * Copyright 2008-2009 Benjamin C. Meyer <ben@meyerhome.net>
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

#ifndef BOOKMARKSMANAGER_H
#define BOOKMARKSMANAGER_H

#include <memory>

#include <qobject.h>

#include <qundostack.h>

class AutoSaver;
class BookmarkNode;
class BookmarksModel;
class QJsonObject;
class BookmarksManager : public QObject
{
    Q_OBJECT

signals:
    void entryAdded(BookmarkNode *item);
    void entryRemoved(BookmarkNode *parent, int row, BookmarkNode *item);
    void entryChanged(BookmarkNode *item);
    // MPROF02: the whole node view was rebuilt — emitted when a stale
    // manager re-binds the shared rust store and reloads its file.
    void bookmarksReset();

public:
    // profileId names the registry profile whose store this manager
    // owns (MPROF02); an empty or non-slug id binds the ambient
    // (focused window's) profile.
    BookmarksManager(QObject *parent = nullptr,
                     const QString &profileId = QString());
    ~BookmarksManager();

    // The application-wide bookmarks store (was
    // BrowserApplication::bookmarksManager()).  Lazily created, owned by
    // qApp so the destructor flushes pending saves at shutdown.
    static BookmarksManager *instance();
    // MPROF02: the store for a specific profile — one instance per
    // registry id, qApp-owned (MPROF05's profile delete tears them
    // down).  Every instance's file lives under profiles/<id>/.
    //
    // rustcore keeps ONE process-global bookmark tree, so only the
    // last-binding manager's nodes are live: touching another
    // profile's manager flushes the current owner to its file and
    // re-loads the new profile's — the file is the source of truth,
    // memory a bound cache.
    static BookmarksManager *instanceFor(const QString &profileId);
    QString profileId() const { return m_profileId; }

    void addBookmark(BookmarkNode *parent, BookmarkNode *node, int row = -1);
    void removeBookmark(BookmarkNode *node);
    void setTitle(BookmarkNode *node, const QString &newTitle);
    void setUrl(BookmarkNode *node, const QString &newUrl);
    void changeExpanded();
    void retranslate() const;

    BookmarkNode *bookmarks();
    BookmarkNode *menu();
    BookmarkNode *toolbar();

    BookmarksModel *bookmarksModel();
    QUndoStack *undoRedoStack() {
        return &m_commands;
    }

#ifdef ARORA_RUSTCORE
    // BKIMP01: the dialog-free import/export cores — public so
    // autotests and smokes can drive them; the slots below wrap them
    // in QFileDialog + summary prompts.
    //
    // importHtmlBookmarks fills a new undoable "Imported <date>"
    // folder under `parent` through rc_bm_import_html (the store's
    // url-scheme gate applies; importBookmarklets is the caller's
    // consent for javascript: urls).  Returns the folder — its
    // store-side children materialize lazily — or nullptr when
    // nothing landed; *summaryOut receives the import counts.
    BookmarkNode *importHtmlBookmarks(const QByteArray &html,
                                    BookmarkNode *parent,
                                    bool importBookmarklets,
                                    QJsonObject *summaryOut);
    // .html -> rc_bm_export_html; anything else is the canonical
    // XBEL writer (rc_bm_save).  False on write failure.
    bool exportBookmarksTo(const QString &fileName);
#endif

public slots:
    void importBookmarks();
    void exportBookmarks();

private slots:
    void save() const;

private:
    void load();
#ifdef ARORA_RUSTCORE
    // Re-binds the process-global rust tree to this profile when the
    // last rc_bm_load targeted another manager's file (m_stale).  All
    // node-touching entry points funnel here first.
    void ensureBound();
    void importHtmlDocument(const QByteArray &html);
#endif

    QString m_profileId;
    bool m_loaded;
    bool m_stale;
    AutoSaver *m_saveTimer;
    std::unique_ptr<BookmarkNode> m_bookmarkRootNode;
    BookmarkNode *m_toolbar;
    BookmarkNode *m_menu;
    BookmarksModel *m_bookmarkModel;
    QUndoStack m_commands;

    friend class RemoveBookmarksCommand;
    friend class ChangeBookmarkCommand;
};

class RemoveBookmarksCommand : public QUndoCommand
{

public:
    RemoveBookmarksCommand(BookmarksManager *m_bookmarkManagaer, BookmarkNode *parent, int row);
    ~RemoveBookmarksCommand();
    void undo() override;
    void redo() override;

protected:
    int m_row;
    BookmarksManager *m_bookmarkManagaer;
    BookmarkNode *m_node;
    BookmarkNode *m_parent;
    bool m_done;
};

class InsertBookmarksCommand : public RemoveBookmarksCommand
{

public:
    InsertBookmarksCommand(BookmarksManager *m_bookmarkManagaer,
                           BookmarkNode *parent, BookmarkNode *node, int row);
    void undo() override {
        RemoveBookmarksCommand::redo();
    }
    void redo() override {
        RemoveBookmarksCommand::undo();
    }

};

class ChangeBookmarkCommand : public QUndoCommand
{

public:
    ChangeBookmarkCommand(BookmarksManager *m_bookmarkManagaer,
                          BookmarkNode *node, const QString &newValue, bool title);
    void undo() override;
    void redo() override;

private:
    BookmarksManager *m_bookmarkManagaer;
    bool m_title;
    QString m_oldValue;
    QString m_newValue;
    BookmarkNode *m_node;
};

#endif // BOOKMARKSMANAGER_H
