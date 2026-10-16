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

#include "notesmodel.h"
#include "profilesettings.h"

#include "autosaver.h"
#include "browserpaths.h"
#include "browserprofile.h"

#include <qdir.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qhash.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qpointer.h>
#include <qsavefile.h>
#include <qsettings.h>
#include <qwebengineprofile.h>

NotesModel::NotesModel(const QString &storageFile, QObject *parent)
    : QAbstractListModel(parent)
    , m_storageFile(storageFile)
    , m_autoSaver(new AutoSaver(this))
{
    load();
}

NotesModel::~NotesModel()
{
    m_autoSaver->saveIfNeccessary();
}

NotesModel *NotesModel::instance(QWebEngineProfile *profile)
{
    if (!profile)
        return nullptr;
    if (profile->isOffTheRecord()) {
        // Volatile per-profile-object store — nothing reaches disk.
        // Parented to the profile so the QPointer expires with it
        // instead of aliasing an address-reused profile object.
        static QHash<QWebEngineProfile *, QPointer<NotesModel>> models;
        NotesModel *model = models.value(profile);
        if (!model) {
            model = new NotesModel(QString(), profile);
            models.insert(profile, model);
        }
        return model;
    }
    // Persistent profiles resolve to their registry persona — a
    // persona's normal and container profiles share one notes store.
    return instanceFor(BrowserProfile::profileIdFor(profile));
}

NotesModel *NotesModel::instanceFor(const QString &profileId)
{
    const QString id = BrowserPaths::isProfileId(profileId)
        ? profileId : BrowserPaths::activeProfileId();
    static QHash<QString, QPointer<NotesModel>> models;
    NotesModel *model = models.value(id);
    if (model)
        return model;

    // <data>/profiles/<id>/notes.json — a file next to the app's
    // own stores rather than inside Chromium's managed storage
    // tree (clearSiteStorage prunes whole directories under the
    // profile path; app files stay out of the blast radius).
    const QString storageFile =
        BrowserPaths::profileDataFilePath(id, QLatin1String("notes.json"));
    if (!QFile::exists(storageFile)) {
        // MPROF02 migration: the pre-per-persona store lived at
        // <data>/notes/<storageName>.json — adopt it if present.
        QString legacyName = id == QLatin1String("default")
            ? QLatin1String("arora") : QLatin1String("arora-") + id;
        const QString legacy =
            BrowserPaths::dataFilePath(QLatin1String("notes"))
            + QLatin1Char('/') + legacyName + QLatin1String(".json");
        if (QFile::exists(legacy)) {
            QDir().mkpath(QFileInfo(storageFile).absolutePath());
            QFile::rename(legacy, storageFile);
        }
    }
    model = new NotesModel(storageFile, qApp);
    migrateLegacyScratch(model, id);
    models.insert(id, model);
    return model;
}

void NotesModel::migrateLegacyScratch(NotesModel *model,
                                      const QString &profileId)
{
    if (!model)
        return;
    ProfileSettings settings(profileId.isEmpty()
        ? BrowserPaths::activeProfileId() : profileId);
    if (!settings.contains(QLatin1String("sidebar/notes")))
        return;
    const QString legacy =
        settings.value(QLatin1String("sidebar/notes")).toString();
    if (!legacy.isEmpty())
        model->insertNote(0, QString(), legacy);
    settings.remove(QLatin1String("sidebar/notes"));
}

int NotesModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_notes.count();
}

QString NotesModel::displayTitle(const QString &title, const QString &body)
{
    if (!title.isEmpty())
        return title;
    const QString firstLine = body.section(QLatin1Char('\n'), 0, 0).trimmed();
    if (!firstLine.isEmpty())
        return firstLine;
    return tr("Untitled");
}

QVariant NotesModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.parent().isValid()
        || index.row() < 0 || index.row() >= m_notes.count())
        return QVariant();
    const Note &note = m_notes.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
        return displayTitle(note.title, note.body);
    case Qt::EditRole:
        return note.title;
    case Qt::ToolTipRole:
        return displayTitle(note.title, note.body);
    case BodyRole:
        return note.body;
    case MarkdownRole:
        return note.markdown;
    }
    return QVariant();
}

bool NotesModel::setData(const QModelIndex &index, const QVariant &value,
                         int role)
{
    if (!index.isValid() || index.parent().isValid()
        || index.row() < 0 || index.row() >= m_notes.count())
        return false;
    Note &note = m_notes[index.row()];
    if (role == Qt::EditRole) {
        const QString title = value.toString();
        if (note.title == title)
            return false;
        note.title = title;
    } else if (role == BodyRole) {
        const QString body = value.toString();
        if (note.body == body)
            return false;
        note.body = body;
    } else if (role == MarkdownRole) {
        const bool markdown = value.toBool();
        if (note.markdown == markdown)
            return false;
        note.markdown = markdown;
    } else {
        return false;
    }
    emit dataChanged(index, index,
                     { Qt::DisplayRole, Qt::EditRole, Qt::ToolTipRole,
                       BodyRole, MarkdownRole });
    markDirty();
    return true;
}

Qt::ItemFlags NotesModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    // Title rename comes through the EditRole editor (list F2 or the
    // editor page's title field).
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable;
}

bool NotesModel::removeRows(int row, int count, const QModelIndex &parent)
{
    if (parent.isValid() || row < 0 || count <= 0
        || row + count > m_notes.count())
        return false;
    beginRemoveRows(parent, row, row + count - 1);
    m_notes.erase(m_notes.begin() + row, m_notes.begin() + row + count);
    endRemoveRows();
    markDirty();
    return true;
}

QModelIndex NotesModel::addNote(const QString &title, const QString &body)
{
    return insertNote(m_notes.count(), title, body);
}

QModelIndex NotesModel::insertNote(int row, const QString &title,
                                   const QString &body)
{
    row = qBound(0, row, m_notes.count());
    beginInsertRows(QModelIndex(), row, row);
    m_notes.insert(row, { title, body });
    endInsertRows();
    markDirty();
    return index(row);
}

QString NotesModel::noteBody(int row) const
{
    return (row >= 0 && row < m_notes.count())
        ? m_notes.at(row).body : QString();
}

bool NotesModel::isPersistent() const
{
    return !m_storageFile.isEmpty();
}

QString NotesModel::storageFile() const
{
    return m_storageFile;
}

void NotesModel::load()
{
    if (m_storageFile.isEmpty())
        return;
    QFile file(m_storageFile);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument doc =
        QJsonDocument::fromJson(file.readAll());
    if (!doc.isArray())
        return;
    const QJsonArray notes = doc.array();
    QList<Note> loaded;
    loaded.reserve(notes.count());
    for (const QJsonValue &value : notes) {
        const QJsonObject object = value.toObject();
        loaded.append({ object.value(QLatin1String("title")).toString(),
                        object.value(QLatin1String("body")).toString(),
                        object.value(QLatin1String("markdown"))
                            .toBool() });
    }
    if (loaded.isEmpty())
        return;
    beginInsertRows(QModelIndex(), 0, loaded.count() - 1);
    m_notes = loaded;
    endInsertRows();
}

void NotesModel::save()
{
    if (m_storageFile.isEmpty())
        return;
    const QString directory =
        QFileInfo(m_storageFile).absolutePath();
    QDir().mkpath(directory);
    QSaveFile file(m_storageFile);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "NotesModel: cannot write" << m_storageFile;
        return;
    }
    QJsonArray notes;
    for (const Note &note : m_notes) {
        notes.append(QJsonObject{
            { QLatin1String("title"), note.title },
            { QLatin1String("body"), note.body },
            { QLatin1String("markdown"), note.markdown },
        });
    }
    file.write(QJsonDocument(notes).toJson(QJsonDocument::Compact));
    file.commit();
}

void NotesModel::markDirty()
{
    m_autoSaver->changeOccurred();
}
