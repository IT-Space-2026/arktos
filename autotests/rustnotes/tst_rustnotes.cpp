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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 */

// RC21 probe: the rustcore notes module owns the same compact JSON
// array NotesModel persists — a file at profiles/<id>/notes.json (or
// an explicit standalone path) of {"body","markdown","title"}
// objects in list order.
//
// Under ARKTOS_RUSTCORE: notes written over the C ABI must load in
// the C++ NotesModel — both the explicit-file lane and the
// persona-bound instanceFor lane — and a C++-authored store must
// decode identically on a fresh FFI handle.  The mirrored-mutation
// contract is the other direction: edits through the Qt model reach
// the store the next ABI open reads.
//
// Under CONFIG+=no-rust: asserts the Qt model keeps the file
// contract alone — the fallback the flag exists for.

#include "qtest_arktos.h"

#include "browserpaths.h"
#include "notesmodel.h"

#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qtemporarydir.h>

#if defined(ARKTOS_RUSTCORE)
#include <rustcore.h>
#endif

class tst_RustNotes : public QObject
{
    Q_OBJECT

private slots:
#if defined(ARKTOS_RUSTCORE)
    void ffiWriteHonoredByCpp();
    void cppWriteHonoredByRust();
    void personaLane();
#else
    void noRustKeepsModel();
#endif
};

#if defined(ARKTOS_RUSTCORE)

namespace {

QString takeString(char *p)
{
    if (!p)
        return QString();
    const QString out = QString::fromUtf8(p);
    rc_string_free(p);
    return out;
}

} // namespace

void tst_RustNotes::ffiWriteHonoredByCpp()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.path() + QLatin1String("/notes.json");

    // Author a store entirely over the ABI: two notes, order plus
    // the markdown flag, committed by sync.
    {
        const QByteArray path = file.toUtf8();
        RcNotes *h = rc_notes_open_file(path.constData());
        QVERIFY(h);
        QCOMPARE(rc_notes_insert(h, 0, "ffi title", "ffi body\nsecond"),
                 0);
        QCOMPARE(rc_notes_insert(h, -9, "lead", ""), 0);
        int changed = -1;
        QCOMPARE(rc_notes_set_markdown(h, 1, 1, &changed), RC_OK);
        QCOMPARE(changed, 1);
        QCOMPARE(rc_notes_is_dirty(h), 1);
        QCOMPARE(rc_notes_sync(h), RC_OK);
        rc_notes_close(h);
    }

    // The C++ model reads the FFI-authored file — ordering and the
    // NOTE03 flag included.
    {
        NotesModel model(file);
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.index(0).data(Qt::EditRole).toString(),
                 QLatin1String("lead"));
        QCOMPARE(model.index(1).data(Qt::EditRole).toString(),
                 QLatin1String("ffi title"));
        QCOMPARE(model.noteBody(1), QLatin1String("ffi body\nsecond"));
        QCOMPARE(model.index(0).data(NotesModel::MarkdownRole).toBool(),
                 false);
        QCOMPARE(model.index(1).data(NotesModel::MarkdownRole).toBool(),
                 true);
    }
}

void tst_RustNotes::cppWriteHonoredByRust()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.path() + QLatin1String("/notes.json");

    // The C++ model authors the store — under ARKTOS_RUSTCORE its
    // save() IS rc_notes_sync, and under no-rust it is the QSaveFile
    // writer the Rust module was shaped to read back.
    {
        NotesModel model(file);
        model.addNote(QLatin1String("cpp title"),
                      QLatin1String("cpp body"));
        model.insertNote(0, QString(), QLatin1String("untitled"));
        QVERIFY(model.setData(model.index(0), true,
                              NotesModel::MarkdownRole));
        model.save();
        QVERIFY(QFileInfo::exists(file));
    }

    // A fresh ABI handle decodes the same rows in the same order.
    {
        const QByteArray path = file.toUtf8();
        RcNotes *h = rc_notes_open_file(path.constData());
        QVERIFY(h);
        QCOMPARE(rc_notes_count(h), 2);
        const QString list =
            takeString(rc_notes_list(h));
        const QJsonArray rows =
            QJsonDocument::fromJson(list.toUtf8()).array();
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows.at(0).toObject().value(QLatin1String("body"))
                     .toString(),
                 QLatin1String("untitled"));
        QCOMPARE(rows.at(0).toObject().value(QLatin1String("markdown"))
                     .toBool(),
                 true);
        QCOMPARE(rows.at(1).toObject().value(QLatin1String("title"))
                     .toString(),
                 QLatin1String("cpp title"));
        rc_notes_close(h);
    }
}

void tst_RustNotes::personaLane()
{
    // The MPROF02 binding: profiles/<id>/notes.json written over the
    // root+profile FFI lane lands in the store instanceFor mints.
    const QString pid = QStringLiteral("notespersona");
    const QByteArray root = BrowserPaths::dataRoot().toUtf8();

    {
        RcNotes *h = rc_notes_open(root.constData(),
                                   pid.toUtf8().constData());
        QVERIFY(h);
        QCOMPARE(rc_notes_insert(h, 0, "persona note", "persona body"),
                 0);
        rc_notes_close(h); // close flushes the dirty store
    }
    const QString file =
        BrowserPaths::profileDataFilePath(pid,
                                          QLatin1String("notes.json"));
    QVERIFY(QFileInfo::exists(file));

    {
        NotesModel *model = NotesModel::instanceFor(pid);
        QVERIFY(model);
        QVERIFY(model->isPersistent());
        QCOMPARE(model->storageFile(), file);
        QCOMPARE(model->rowCount(), 1);
        QCOMPARE(model->index(0).data(Qt::EditRole).toString(),
                 QLatin1String("persona note"));

        // A mirrored C++ edit reaches the next FFI open.
        model->setData(model->index(0), QLatin1String("renamed"),
                       Qt::EditRole);
        model->save();
        NotesModel::releaseFor(pid);
    }

    {
        RcNotes *h = rc_notes_open(root.constData(),
                                   pid.toUtf8().constData());
        QVERIFY(h);
        const QString row = takeString(rc_notes_get(h, 0));
        QCOMPARE(QJsonDocument::fromJson(row.toUtf8())
                     .object()
                     .value(QLatin1String("title"))
                     .toString(),
                 QLatin1String("renamed"));
        rc_notes_close(h);
    }
}

#else // !ARKTOS_RUSTCORE — the fallback contract

void tst_RustNotes::noRustKeepsModel()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.path() + QLatin1String("/notes.json");
    {
        NotesModel model(file);
        model.addNote(QLatin1String("legacy"),
                      QLatin1String("legacy body"));
        model.save();
        QVERIFY(QFileInfo::exists(file));
    }
    {
        NotesModel model(file);
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.index(0).data(Qt::EditRole).toString(),
                 QLatin1String("legacy"));
    }
}

#endif

QTEST_MAIN(tst_RustNotes)
#include "tst_rustnotes.moc"
