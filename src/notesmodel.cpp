/*
 * Copyright 2026 Arktos Browser - IT-Space
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

#if defined(ARKTOS_RUSTCORE)
#include "rustcorebridge.h"

#include <arktos_core.h>
#endif

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

NotesModel::NotesModel(const QString &storageFile, QObject *parent,
                       const QString &profileId)
    : QAbstractListModel(parent)
    , m_storageFile(storageFile)
    , m_autoSaver(new AutoSaver(this))
{
#if !defined(ARKTOS_RUSTCORE)
    Q_UNUSED(profileId);
#endif
#if defined(ARKTOS_RUSTCORE)
    // RC21: the Rust store owns the file.  A persona-bound model
    // mints through the shared AcCore when the bridge can serve —
    // the facade binds the same profiles/<id>/notes.json the caller
    // computed — and any other binding takes the explicit-file lane
    // (an empty file mints the volatile off-the-record store).
    // A mint failure leaves m_store NULL and the Qt file I/O below
    // is the fallback, same as CONFIG+=no-rust.
    if (!profileId.isEmpty()) {
        if (RustCoreBridge *bridge = RustCoreBridge::serving()) {
            m_store = bridge->notes(profileId);
            m_facadeMinted = (m_store != nullptr);
        }
    }
    if (!m_store) {
        const QByteArray path = m_storageFile.toUtf8();
        m_store = rc_notes_open_file(
            m_storageFile.isEmpty() ? nullptr : path.constData());
    }
#endif
    load();
}

NotesModel::~NotesModel()
{
    m_autoSaver->saveIfNeccessary();
#if defined(ARKTOS_RUSTCORE)
    if (m_store) {
        if (m_facadeMinted)
            ac_notes_close(m_store);
        else
            rc_notes_close(m_store);
    }
#endif
}

NotesModel *NotesModel::instance(QWebEngineProfile *profile)
{
    if (!profile)
        return nullptr;
    if (profile->isOffTheRecord()) {
        // Volatile per-profile-object store — nothing reaches disk.
        // Parented to the profile so the QPointer expires with it
        // instead of aliasing an address-reused profile object; the
        // hash row goes with it too so OTR churn cannot grow the map.
        static QHash<QWebEngineProfile *, QPointer<NotesModel>> models;
        NotesModel *model = models.value(profile);
        if (!model) {
            model = new NotesModel(QString(), profile);
            models.insert(profile, model);
            QObject::connect(profile, &QObject::destroyed, profile,
                             [profile]() { models.remove(profile); });
        }
        return model;
    }
    // Persistent profiles resolve to their registry persona — a
    // persona's normal and container profiles share one notes store.
    return instanceFor(BrowserProfile::profileIdFor(profile));
}

static QHash<QString, QPointer<NotesModel>> &notesModels()
{
    static QHash<QString, QPointer<NotesModel>> models;
    return models;
}

NotesModel *NotesModel::instanceFor(const QString &profileId)
{
    const QString id = BrowserPaths::isProfileId(profileId)
        ? profileId : BrowserPaths::activeProfileId();
    QHash<QString, QPointer<NotesModel>> &models = notesModels();
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
        // BRAND01: a pre-rebrand tree carries the arora-<name>
        // spelling, so check both identities' file names.
        const QStringList legacyNames = {
            id == QLatin1String("default")
                ? QLatin1String("arktos") : QLatin1String("arktos-") + id,
            id == QLatin1String("default")
                ? QLatin1String("arora") : QLatin1String("arora-") + id,
        };
        for (const QString &legacyName : legacyNames) {
            const QString legacy =
                BrowserPaths::dataFilePath(QLatin1String("notes"))
                + QLatin1Char('/') + legacyName + QLatin1String(".json");
            if (QFile::exists(legacy)) {
                QDir().mkpath(QFileInfo(storageFile).absolutePath());
                // CA12/W4: QFile::rename refuses an existing target
                // on every platform and can lose to a transient
                // handle — fall back to copy+remove so the legacy
                // store still migrates instead of being skipped.
                if (!QFile::rename(legacy, storageFile)
                        && !QFile::exists(storageFile)
                        && QFile::copy(legacy, storageFile))
                    QFile::remove(legacy);
                break;
            }
        }
    }
    model = new NotesModel(storageFile, qApp, id);
    migrateLegacyScratch(model, id);
    models.insert(id, model);
    return model;
}

void NotesModel::releaseFor(const QString &profileId)
{
    // Strict id check — no ambient-persona aliasing; releasing the
    // wrong persona's store is worse than leaking it.
    if (!BrowserPaths::isProfileId(profileId))
        return;
    if (NotesModel *model = notesModels().take(profileId))
        delete model;
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
#if defined(ARKTOS_RUSTCORE)
    // Mirror the accepted write into the Rust store — the equality
    // checks above already did its changed/no-change split.
    if (m_store) {
        int changed = 0;
        const QByteArray bytes = value.toString().toUtf8();
        if (role == Qt::EditRole)
            rc_notes_set_title(m_store, index.row(),
                               bytes.constData(), &changed);
        else if (role == BodyRole)
            rc_notes_set_body(m_store, index.row(),
                              bytes.constData(), &changed);
        else
            rc_notes_set_markdown(m_store, index.row(),
                                  value.toBool() ? 1 : 0, &changed);
    }
#endif
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
#if defined(ARKTOS_RUSTCORE)
    if (m_store)
        rc_notes_remove(m_store, row, count);
#endif
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
#if defined(ARKTOS_RUSTCORE)
    if (m_store)
        rc_notes_insert(m_store, row, title.toUtf8().constData(),
                        body.toUtf8().constData());
#endif
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
    QByteArray bytes;
#if defined(ARKTOS_RUSTCORE)
    // The Rust store decoded the file at open — hydrate the Qt-side
    // cache from it rather than re-reading the file.
    if (m_store) {
        if (char *json = rc_notes_list(m_store)) {
            bytes = QByteArray(json);
            rc_string_free(json);
        }
    } else
#endif
    if (!m_storageFile.isEmpty()) {
        QFile file(m_storageFile);
        if (file.open(QIODevice::ReadOnly))
            bytes = file.readAll();
    }
    if (bytes.isEmpty())
        return;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes);
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
#if defined(ARKTOS_RUSTCORE)
    if (m_store) {
        // rc_notes_sync commits atomically (temp+rename) like the
        // QSaveFile below and creates the parent dir like the
        // mkpath; the volatile lane is a no-op.
        if (rc_notes_sync(m_store) != RC_OK) {
            char *err = rc_last_error_message();
            qWarning() << "NotesModel: cannot write" << m_storageFile
                       << QString::fromUtf8(err ? err : "");
            rc_string_free(err);
        }
        return;
    }
#endif
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
