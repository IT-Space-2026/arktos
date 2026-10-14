/*
 * Copyright 2026 The Arora Authors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

// NOTE02: NotesModel — the per-profile multi-note store behind the
// sidebar's Notes page.  Covers the model contract (via
// QAbstractItemModelTester), add/rename/select/delete, the JSON
// persistence round-trip, the volatile off-the-record store and the
// sidebar/notes scratch-pad migration.

#include <QtTest/QtTest>
#include "qtest_arora.h"

#include <modeltest.h>
#include <notesmodel.h>
#include <browserprofile.h>

#include <qfile.h>
#include <qfileinfo.h>
#include <qsettings.h>
#include <qtemporarydir.h>
#include <qwebengineprofile.h>

class tst_NotesModel : public QObject
{
    Q_OBJECT

public slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

private slots:
    void modelContract();
    void addRemove();
    void titles();
    void persistence();
    void volatileModel();
    void migration();
    void markdownFlag();
    void instancePerProfile();
};

void tst_NotesModel::initTestCase()
{
}

void tst_NotesModel::cleanupTestCase()
{
}

void tst_NotesModel::init()
{
    QSettings().remove(QLatin1String("sidebar/notes"));
}

void tst_NotesModel::cleanup()
{
    QSettings().remove(QLatin1String("sidebar/notes"));
}

// The QAbstractItemModelTester leg — run a mixed add/edit/remove
// sequence while the tester watches the row/role bookkeeping.
void tst_NotesModel::modelContract()
{
    NotesModel model{ QString() };
    new QAbstractItemModelTester(&model,
        QAbstractItemModelTester::FailureReportingMode::QtTest, &model);
    const QModelIndex first = model.addNote(QLatin1String("one"),
                                          QLatin1String("first body"));
    model.addNote(QLatin1String("two"), QLatin1String("second body"));
    model.insertNote(0, QLatin1String("zero"), QLatin1String("lead"));
    model.setData(model.index(1), QLatin1String("one renamed"),
                  Qt::EditRole);
    model.setData(first, QLatin1String("edited body"),
                  NotesModel::BodyRole);
    model.removeRow(0);
    model.removeRow(model.rowCount() - 1);
    QCOMPARE(model.rowCount(), 1);
}

void tst_NotesModel::addRemove()
{
    NotesModel model{ QString() };
    QCOMPARE(model.rowCount(), 0);

    const QModelIndex a = model.addNote(QLatin1String("A"),
                                        QLatin1String("body a"));
    const QModelIndex c = model.addNote(QLatin1String("C"),
                                        QLatin1String("body c"));
    QCOMPARE(model.rowCount(), 2);
    // insertNote lands at the requested position.
    model.insertNote(1, QLatin1String("B"), QLatin1String("body b"));
    QCOMPARE(model.index(0).data(Qt::DisplayRole).toString(),
             QLatin1String("A"));
    QCOMPARE(model.index(1).data(Qt::DisplayRole).toString(),
             QLatin1String("B"));
    QCOMPARE(model.index(2).data(Qt::DisplayRole).toString(),
             QLatin1String("C"));
    QCOMPARE(model.noteBody(1), QLatin1String("body b"));

    // Out-of-range removals are refused; a valid one shifts rows.
    QVERIFY(!model.removeRow(-1));
    QVERIFY(!model.removeRow(3));
    QVERIFY(!model.removeRows(1, 5));
    QVERIFY(model.removeRow(1));
    QCOMPARE(model.rowCount(), 2);
    QCOMPARE(model.index(1).data(Qt::DisplayRole).toString(),
             QLatin1String("C"));
    QCOMPARE(model.noteBody(0), QLatin1String("body a"));
    QCOMPARE(model.noteBody(2), QString());
    Q_UNUSED(a);
    Q_UNUSED(c);
}

void tst_NotesModel::titles()
{
    NotesModel model{ QString() };
    // An untitled note displays its body's first line; a blank note
    // falls back to "Untitled".
    model.addNote(QString(), QLatin1String("first line\nsecond line"));
    QCOMPARE(model.index(0).data(Qt::DisplayRole).toString(),
             QLatin1String("first line"));
    // EditRole exposes the raw (empty) title so the editor shows a
    // blank field, not the derived placeholder.
    QCOMPARE(model.index(0).data(Qt::EditRole).toString(), QString());
    model.addNote();
    QCOMPARE(model.index(1).data(Qt::DisplayRole).toString(),
             QLatin1String("Untitled"));

    // Rename-via-title: EditRole writes the title and repaints the
    // row's display text.
    QVERIFY(model.setData(model.index(0), QLatin1String("Renamed"),
                          Qt::EditRole));
    QCOMPARE(model.index(0).data(Qt::DisplayRole).toString(),
             QLatin1String("Renamed"));
    QCOMPARE(model.index(0).data(Qt::EditRole).toString(),
             QLatin1String("Renamed"));

    // Body edits flow through BodyRole and re-derive an untitled
    // display name.
    QVERIFY(model.setData(model.index(1), QLatin1String("typed body"),
                          NotesModel::BodyRole));
    QCOMPARE(model.index(1).data(Qt::DisplayRole).toString(),
             QLatin1String("typed body"));

    // Refused writes.
    QVERIFY(!model.setData(model.index(5), QLatin1String("x"),
                           Qt::EditRole));
    QVERIFY(!model.setData(model.index(0), QLatin1String("x"),
                           Qt::CheckStateRole));

    // List rows are editable (the title rename path).
    QVERIFY(model.flags(model.index(0)) & Qt::ItemIsEditable);
    QCOMPARE(model.flags(QModelIndex()), Qt::NoItemFlags);
}

void tst_NotesModel::persistence()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.path() + QLatin1String("/notes.json");

    {
        NotesModel model(file);
        QVERIFY(model.isPersistent());
        QCOMPARE(model.storageFile(), file);
        model.addNote(QLatin1String("Shopping"),
                      QLatin1String("milk\neggs"));
        model.addNote(QString(), QLatin1String("untitled body"));
        model.save();
        QVERIFY(QFileInfo::exists(file));
    }
    {
        // A fresh model over the same file reloads every note.
        NotesModel model(file);
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.index(0).data(Qt::EditRole).toString(),
                 QLatin1String("Shopping"));
        QCOMPARE(model.noteBody(0), QLatin1String("milk\neggs"));
        QCOMPARE(model.index(1).data(Qt::DisplayRole).toString(),
                 QLatin1String("untitled body"));

        // Edits and deletes persist through the next load too.
        model.setData(model.index(0), QLatin1String("Groceries"),
                      Qt::EditRole);
        model.removeRow(1);
        model.save();
    }
    {
        NotesModel model(file);
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.index(0).data(Qt::EditRole).toString(),
                 QLatin1String("Groceries"));
        QCOMPARE(model.noteBody(0), QLatin1String("milk\neggs"));
    }

    // A missing directory is created on save.
    const QString nested =
        dir.path() + QLatin1String("/deep/dir/notes.json");
    {
        NotesModel model(nested);
        model.addNote(QLatin1String("x"));
        model.save();
        QVERIFY(QFileInfo::exists(nested));
    }
}

void tst_NotesModel::volatileModel()
{
    // An empty storage file = the off-the-record store: load/save are
    // no-ops and nothing touches disk.
    NotesModel model{ QString() };
    QVERIFY(!model.isPersistent());
    QCOMPARE(model.storageFile(), QString());
    model.addNote(QLatin1String("private"), QLatin1String("secret"));
    model.save();   // must not throw or write
    QCOMPARE(model.rowCount(), 1);
}

void tst_NotesModel::migration()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.path() + QLatin1String("/notes.json");

    // The pre-NOTE02 scratch pad lands as note #1 and the key is
    // consumed so a second store can't migrate it twice.
    QSettings().setValue(QLatin1String("sidebar/notes"),
                         QLatin1String("scratch pad\ncontent"));
    {
        NotesModel model(file);
        model.addNote(QLatin1String("existing"),
                      QLatin1String("earlier note"));
        NotesModel::migrateLegacyScratch(&model);
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.noteBody(0),
                 QLatin1String("scratch pad\ncontent"));
        // An untitled migrated note displays its own first line.
        QCOMPARE(model.index(0).data(Qt::DisplayRole).toString(),
                 QLatin1String("scratch pad"));
        QCOMPARE(model.index(1).data(Qt::EditRole).toString(),
                 QLatin1String("existing"));
        QVERIFY(!QSettings().contains(QLatin1String("sidebar/notes")));
        model.save();
    }
    {
        // The migrated note reloads from the file — nothing is lost.
        NotesModel model(file);
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.noteBody(0),
                 QLatin1String("scratch pad\ncontent"));
    }

    // An empty legacy value migrates nothing but still retires the
    // key; re-running on a store that already has notes is a no-op.
    QSettings().setValue(QLatin1String("sidebar/notes"), QString());
    NotesModel model(file);
    QCOMPARE(model.rowCount(), 2);
    NotesModel::migrateLegacyScratch(&model);
    QCOMPARE(model.rowCount(), 2);
    QVERIFY(!QSettings().contains(QLatin1String("sidebar/notes")));

    // A null model argument is tolerated.
    NotesModel::migrateLegacyScratch(nullptr);
}

void tst_NotesModel::markdownFlag()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.path() + QLatin1String("/notes.json");

    // NOTE03: the per-note markdown flag rides MarkdownRole — false
    // by default, writable, and persisted through the JSON store.
    {
        NotesModel model(file);
        model.addNote(QLatin1String("md"), QLatin1String("**bold**"));
        model.addNote(QLatin1String("plain"), QLatin1String("plain"));
        QCOMPARE(model.index(0).data(NotesModel::MarkdownRole).toBool(),
                 false);
        QVERIFY(model.setData(model.index(0), true,
                              NotesModel::MarkdownRole));
        QCOMPARE(model.index(0).data(NotesModel::MarkdownRole).toBool(),
                 true);
        QCOMPARE(model.index(1).data(NotesModel::MarkdownRole).toBool(),
                 false);
        // Same-value writes are refused like the other roles'.
        QVERIFY(!model.setData(model.index(0), true,
                               NotesModel::MarkdownRole));
        model.save();
    }
    {
        NotesModel model(file);
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.index(0).data(NotesModel::MarkdownRole).toBool(),
                 true);
        QCOMPARE(model.index(1).data(NotesModel::MarkdownRole).toBool(),
                 false);
    }

    // A pre-NOTE03 store without the key loads the flag as false.
    {
        QFile legacy(file);
        QVERIFY(legacy.open(QIODevice::WriteOnly | QIODevice::Truncate));
        legacy.write("[{\"title\":\"old\",\"body\":\"still here\"}]");
        legacy.close();
        NotesModel model(file);
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.index(0).data(NotesModel::MarkdownRole).toBool(),
                 false);
    }
}

void tst_NotesModel::instancePerProfile()
{
    // The shared normal-profile store: same object on repeat calls,
    // persistent (the named "arora" profile has a storageName).
    NotesModel *normal = NotesModel::instance(BrowserProfile::normalProfile());
    QVERIFY(normal);
    QVERIFY(normal->isPersistent());
    QCOMPARE(NotesModel::instance(BrowserProfile::normalProfile()),
             normal);
    QVERIFY(normal->storageFile().endsWith(QLatin1String("arora.json")));

    // An unnamed (off-the-record) profile gets a distinct volatile
    // store — private notes never reach disk.
    QWebEngineProfile *otr = new QWebEngineProfile(qApp);
    NotesModel *volatileStore = NotesModel::instance(otr);
    QVERIFY(volatileStore);
    QVERIFY(volatileStore != normal);
    QVERIFY(!volatileStore->isPersistent());
    QCOMPARE(NotesModel::instance(otr), volatileStore);

    QVERIFY(!NotesModel::instance(nullptr));
}

QTEST_MAIN(tst_NotesModel)
#include "tst_notesmodel.moc"
