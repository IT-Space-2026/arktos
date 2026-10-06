/*
 * Copyright 2026 The Arora Authors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

// COV02: BookmarksModel — the tree model over BookmarksManager used by
// the bookmarks dialog/menu/toolbar.  Pins the node<->index mapping,
// role data, flags, add/remove propagation through the undo-stack
// commands, mime drag+drop, plus model-consistency via
// QAbstractItemModelTester.

#include <QtTest/QtTest>
#include <modeltest.h>
#include <bookmarksmanager.h>
#include <bookmarksmodel.h>
#include <bookmarknode.h>

class tst_BookmarksModel : public QObject
{
    Q_OBJECT

public slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

private slots:
    void bookmarksmodel();
    void addRemove();
    void dataRoles();
    void flags();
    void setData();
    void mime();
    void dropUrls();
    void indexForNode();
};

static BookmarkNode *makeBookmark(const QString &title, const QString &url)
{
    BookmarkNode *node = new BookmarkNode(BookmarkNode::Bookmark);
    node->title = title;
    node->url = url;
    return node;
}

// The bundled defaultbookmarks.xbel pre-populates the menu folder;
// tests that assert absolute row counts start from an empty one.
static void clearFolder(BookmarksManager &manager, BookmarkNode *folder)
{
    while (!folder->children().isEmpty())
        manager.removeBookmark(folder->children().first());
}

void tst_BookmarksModel::initTestCase()
{
    QCoreApplication::setApplicationName("tst_bookmarksmodel");
    // Keep the manager's bookmarks.xbel writes out of the real data dir.
    QStandardPaths::setTestModeEnabled(true);
}

void tst_BookmarksModel::cleanupTestCase()
{
}

void tst_BookmarksModel::init()
{
}

void tst_BookmarksModel::cleanup()
{
}

void tst_BookmarksModel::bookmarksmodel()
{
    BookmarksManager manager;
    BookmarksModel *model = manager.bookmarksModel();
    ModelTest tester(model);

    QCOMPARE(model->bookmarksManager(), &manager);
    QCOMPARE(model->columnCount(), 2);
    QCOMPARE(model->headerData(0, Qt::Horizontal).toString(), QLatin1String("Title"));
    QCOMPARE(model->headerData(1, Qt::Horizontal).toString(), QLatin1String("Address"));
    QCOMPARE(model->supportedDropActions(), Qt::CopyAction | Qt::MoveAction);

    // The default root holds the toolbar and menu folders.
    QCOMPARE(model->rowCount(), 2);
    QCOMPARE(model->node(QModelIndex()), manager.bookmarks());
    QVERIFY(model->hasChildren());
    QVERIFY(model->hasChildren(model->index(0, 0)));
}

void tst_BookmarksModel::addRemove()
{
    BookmarksManager manager;
    BookmarksModel *model = manager.bookmarksModel();
    ModelTest tester(model);

    BookmarkNode *menu = manager.menu();
    clearFolder(manager, menu);
    QModelIndex menuIndex = model->index(menu);
    QVERIFY(menuIndex.isValid());
    QCOMPARE(model->rowCount(menuIndex), 0);

    BookmarkNode *bookmark = makeBookmark(QLatin1String("Arora"),
                                          QLatin1String("https://arora-browser.org"));
    manager.addBookmark(menu, bookmark);
    QCOMPARE(model->rowCount(menuIndex), 1);

    QModelIndex child = model->index(0, 0, menuIndex);
    QCOMPARE(model->node(child), bookmark);
    QCOMPARE(model->data(child).toString(), QLatin1String("Arora"));
    QCOMPARE(model->data(model->index(0, 1, menuIndex)).toString(),
             QLatin1String("https://arora-browser.org"));

    // The removal skips nothing here (only menu/toolbar folders are
    // protected) and routes through the undo stack.
    QVERIFY(model->removeRows(0, 1, menuIndex));
    QCOMPARE(model->rowCount(menuIndex), 0);
    manager.undoRedoStack()->undo();
    QCOMPARE(model->rowCount(menuIndex), 1);

    // The menu/toolbar folders are skipped by removeRows — the call
    // returns true but the folder survives.
    int rootRows = model->rowCount();
    QVERIFY(model->removeRows(menuIndex.row(), 1));
    QCOMPARE(model->rowCount(), rootRows);
}

void tst_BookmarksModel::dataRoles()
{
    BookmarksManager manager;
    BookmarksModel *model = manager.bookmarksModel();
    ModelTest tester(model);

    BookmarkNode *menu = manager.menu();
    clearFolder(manager, menu);
    QModelIndex menuIndex = model->index(menu);

    BookmarkNode *bookmark = makeBookmark(QLatin1String("Arora"),
                                          QLatin1String("https://arora-browser.org"));
    BookmarkNode *separator = new BookmarkNode(BookmarkNode::Separator);
    manager.addBookmark(menu, bookmark);
    manager.addBookmark(menu, separator, 0);
    QCOMPARE(model->rowCount(menuIndex), 2);

    QModelIndex first = model->index(0, 0, menuIndex);
    QCOMPARE(model->data(first, BookmarksModel::SeparatorRole).toBool(), true);
    QCOMPARE(model->data(first, BookmarksModel::TypeRole).toInt(),
             int(BookmarkNode::Separator));
    // Separators render as a dotted line in column 0.
    QCOMPARE(model->data(first).toString(), QString(50, QChar(0xB7)));

    QModelIndex second = model->index(1, 0, menuIndex);
    QCOMPARE(model->data(second, BookmarksModel::SeparatorRole).toBool(), false);
    QCOMPARE(model->data(second, BookmarksModel::TypeRole).toInt(),
             int(BookmarkNode::Bookmark));
    QCOMPARE(model->data(second, BookmarksModel::UrlStringRole).toString(),
             QLatin1String("https://arora-browser.org"));
    QCOMPARE(model->data(second, BookmarksModel::UrlRole).toUrl(),
             QUrl(QLatin1String("https://arora-browser.org")));
    QVERIFY(model->data(second, Qt::DecorationRole).isValid());
    QVERIFY(model->data(first, Qt::DecorationRole).isValid());
}

void tst_BookmarksModel::flags()
{
    BookmarksManager manager;
    BookmarksModel *model = manager.bookmarksModel();

    BookmarkNode *menu = manager.menu();
    clearFolder(manager, menu);
    QModelIndex menuIndex = model->index(menu);

    // Folders accept drops but the menu/toolbar folders themselves
    // are not draggable or editable.
    Qt::ItemFlags menuFlags = model->flags(menuIndex);
    QVERIFY(menuFlags & Qt::ItemIsDropEnabled);
    QVERIFY(!(menuFlags & Qt::ItemIsDragEnabled));
    QVERIFY(!(menuFlags & Qt::ItemIsEditable));

    BookmarkNode *bookmark = makeBookmark(QLatin1String("Arora"),
                                          QLatin1String("https://arora-browser.org"));
    manager.addBookmark(menu, bookmark);
    QModelIndex child0 = model->index(0, 0, menuIndex);
    QModelIndex child1 = model->index(0, 1, menuIndex);
    QVERIFY(model->flags(child0) & Qt::ItemIsDragEnabled);
    QVERIFY(model->flags(child0) & Qt::ItemIsEditable);
    QVERIFY(model->flags(child1) & Qt::ItemIsEditable);
    // A leaf bookmark is not a drop target.
    QVERIFY(!(model->flags(child0) & Qt::ItemIsDropEnabled));

    QCOMPARE(model->flags(QModelIndex()), Qt::NoItemFlags);
}

void tst_BookmarksModel::setData()
{
    BookmarksManager manager;
    BookmarksModel *model = manager.bookmarksModel();
    ModelTest tester(model);

    clearFolder(manager, manager.menu());
    BookmarkNode *bookmark = makeBookmark(QLatin1String("Old"),
                                          QLatin1String("https://old.example.com"));
    manager.addBookmark(manager.menu(), bookmark);
    QModelIndex menuIndex = model->index(manager.menu());

    QVERIFY(model->setData(model->index(0, 0, menuIndex), QLatin1String("New")));
    QCOMPARE(bookmark->title, QLatin1String("New"));
    QVERIFY(model->setData(model->index(0, 1, menuIndex),
                           QLatin1String("https://new.example.com")));
    QCOMPARE(bookmark->url, QLatin1String("https://new.example.com"));
    QVERIFY(model->setData(model->index(0, 0, menuIndex),
                           QUrl(QLatin1String("https://role.example.com")),
                           BookmarksModel::UrlRole));
    QCOMPARE(bookmark->url, QLatin1String("https://role.example.com"));

    QVERIFY(!model->setData(QModelIndex(), QLatin1String("x")));
    // Edits go through the undo stack.
    manager.undoRedoStack()->undo();
    QCOMPARE(bookmark->url, QLatin1String("https://new.example.com"));
}

void tst_BookmarksModel::mime()
{
    BookmarksManager manager;
    BookmarksModel *model = manager.bookmarksModel();

    QStringList types = model->mimeTypes();
    QVERIFY(types.contains(QLatin1String("application/bookmarks.xbel")));
    QVERIFY(types.contains(QLatin1String("text/uri-list")));

    clearFolder(manager, manager.menu());
    BookmarkNode *bookmark = makeBookmark(QLatin1String("Arora"),
                                          QLatin1String("https://arora-browser.org"));
    manager.addBookmark(manager.menu(), bookmark);
    QModelIndex menuIndex = model->index(manager.menu());
    QModelIndex child = model->index(0, 0, menuIndex);

    QMimeData *mime = model->mimeData(QModelIndexList() << child);
    QVERIFY(mime->hasFormat(QLatin1String("application/bookmarks.xbel")));
    QCOMPARE(mime->urls().count(), 1);
    QCOMPARE(mime->urls().first(),
             QUrl(QLatin1String("https://arora-browser.org")));
    delete mime;
}

// Dropping a text/uri-list creates a bookmark under the target folder.
void tst_BookmarksModel::dropUrls()
{
    BookmarksManager manager;
    BookmarksModel *model = manager.bookmarksModel();
    ModelTest tester(model);

    clearFolder(manager, manager.menu());
    QModelIndex menuIndex = model->index(manager.menu());

    QMimeData mime;
    mime.setUrls(QList<QUrl>() << QUrl(QLatin1String("https://dropped.example.com")));
    mime.setText(QLatin1String("Dropped"));
    QVERIFY(model->dropMimeData(&mime, Qt::CopyAction, -1, 0, menuIndex));
    QCOMPARE(model->rowCount(menuIndex), 1);
    QCOMPARE(model->data(model->index(0, 0, menuIndex)).toString(),
             QLatin1String("Dropped"));

    // Column > 0 and mime-data without urls are both refused.
    QVERIFY(!model->dropMimeData(&mime, Qt::CopyAction, -1, 1, menuIndex));
    QMimeData empty;
    QVERIFY(!model->dropMimeData(&empty, Qt::CopyAction, -1, 0, menuIndex));
    // IgnoreAction is a no-op success.
    QVERIFY(model->dropMimeData(&empty, Qt::IgnoreAction, -1, 0, menuIndex));
    QCOMPARE(model->rowCount(menuIndex), 1);
}

void tst_BookmarksModel::indexForNode()
{
    BookmarksManager manager;
    BookmarksModel *model = manager.bookmarksModel();

    // node<->index round-trips through the tree.
    BookmarkNode *menu = manager.menu();
    clearFolder(manager, menu);
    QModelIndex menuIndex = model->index(menu);
    QCOMPARE(model->node(menuIndex), menu);

    BookmarkNode *folder = new BookmarkNode(BookmarkNode::Folder);
    folder->title = QLatin1String("Folder");
    manager.addBookmark(menu, folder);
    BookmarkNode *bookmark = makeBookmark(QLatin1String("Deep"),
                                          QLatin1String("https://deep.example.com"));
    manager.addBookmark(folder, bookmark);

    QModelIndex folderIndex = model->index(folder);
    QCOMPARE(folderIndex.parent(), menuIndex);
    QModelIndex deepIndex = model->index(bookmark);
    QCOMPARE(deepIndex.parent(), folderIndex);
    QCOMPARE(model->node(deepIndex), bookmark);

    // The root has no index.
    QCOMPARE(model->index(manager.bookmarks()), QModelIndex());
}

QTEST_MAIN(tst_BookmarksModel)
#include "tst_bookmarksmodel.moc"
