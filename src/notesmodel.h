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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

#ifndef NOTESMODEL_H
#define NOTESMODEL_H

#include <qabstractitemmodel.h>
#include <qlist.h>
#include <qstring.h>

class AutoSaver;
class QWebEngineProfile;

// NOTE02: the sidebar's multi-note store — one shared list model per
// browsing profile.  A note is a title plus free-form body text; an
// empty title displays as the body's first line ("Untitled" when both
// are empty) so notes never look blank in the list.
//
// Persistence is a per-profile JSON file under the app data dir
// (<data>/notes/<profile storageName>.json), debounce-written through
// the model's own AutoSaver like every other app-side store.  An
// off-the-record profile shares a session-only in-memory model (an
// empty storage file): private/tor notes never reach disk.
class NotesModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role {
        BodyRole = Qt::UserRole + 1,
        // NOTE03: per-note markdown flag (bool) — the sidebar editor
        // renders a flagged note's body in place.
        MarkdownRole
    };

    // A standalone store bound to an explicit file — the form tests
    // and any future per-path caller use.  An empty storageFile makes
    // the model volatile: load/save become no-ops.
    explicit NotesModel(const QString &storageFile,
                        QObject *parent = nullptr);
    ~NotesModel() override;

    // The shared notes model for a browsing profile — one instance per
    // profile object, qApp-owned, so every window's panel edits the
    // same set.  nullptr in -> nullptr out.
    static NotesModel *instance(QWebEngineProfile *profile);

    // NOTE02 migration seam: folds the pre-multi-note scratch pad
    // (the sidebar/notes QSettings key) into this store as the first
    // note, then removes the key so no other profile's store picks it
    // up twice.  Safe to call on any model — no key, no-op.
    static void migrateLegacyScratch(NotesModel *model);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index,
                  int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex &index, const QVariant &value,
                 int role = Qt::EditRole) override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    bool removeRows(int row, int count,
                    const QModelIndex &parent = QModelIndex()) override;

    // What a row shows when its stored title is empty — the body's
    // first line, or "Untitled" for a blank note.
    static QString displayTitle(const QString &title, const QString &body);

    // Appends a note and returns its index.
    QModelIndex addNote(const QString &title = QString(),
                        const QString &body = QString());
    // Inserts at a position (used by migrateLegacyScratch to land the
    // old scratch text as note #1) and returns the new index.
    QModelIndex insertNote(int row, const QString &title,
                           const QString &body);

    QString noteBody(int row) const;

    bool isPersistent() const;
    QString storageFile() const;

public slots:
    // AutoSaver target — writes the JSON store.  Volatile (off the
    // record) models no-op.
    void save();

private:
    struct Note {
        QString title;
        QString body;
        bool markdown = false;
    };

    void load();
    void markDirty();

    QList<Note> m_notes;
    QString m_storageFile;
    AutoSaver *m_autoSaver;
};

#endif // NOTESMODEL_H
