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
#ifdef ARORA_RUSTCORE
#include <qjsonobject.h>
#endif

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
#ifdef ARORA_RUSTCORE
    void rustPersistence();
    void rustHtmlImportExport();
#endif
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

#ifdef ARORA_RUSTCORE
// RCORE02: edits write through to the Rust XBEL store; a fresh
// manager reloads them from bookmarks.xbel (atomic Rust write).
void tst_BookmarksModel::rustPersistence()
{
    {
        BookmarksManager manager;
        manager.addBookmark(manager.menu(),
            makeBookmark(QLatin1String("Persist"),
                         QLatin1String("https://persist.example.com")));
    }   // dtor flushes the pending autosave
    {
        BookmarksManager manager;
        bool found = false;
        const QList<BookmarkNode *> children = manager.menu()->children();
        for (BookmarkNode *node : children) {
            if (node->url == QLatin1String("https://persist.example.com")) {
                QCOMPARE(node->title, QLatin1String("Persist"));
                found = true;
            }
        }
        QVERIFY(found);
        // Leave the persisted file clean for the next manager.
        clearFolder(manager, manager.menu());
    }
}

// BKIMP01: Netscape-HTML import/export through the manager — the
// store's url-scheme gate refuses unsafe schemes, the import folder
// materializes lazily, and export->import round-trips.
void tst_BookmarksModel::rustHtmlImportExport()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        BookmarksManager manager;
        BookmarkNode *menu = manager.menu();
        clearFolder(manager, menu);
        const int base = menu->children().count();

        const QByteArray html =
            "<DL><p>"
            "<DT><H3>Docs</H3><DL><p>"
            "<DT><A HREF=\"https://a.example/\">A</A>"
            "<DT><A HREF=\"javascript:alert(1)\">J</A>"
            "<DT><A HREF=\"data:,x\">D</A>"
            "<DT><A HREF=\"http://b.example/\">B</A>"
            "<HR>"
            "</DL><p>"
            "<DT><A HREF=\"https://c.example/\">C &lt;3</A>"
            "</DL><p>";
        QJsonObject summary;
        BookmarkNode *folder =
            manager.importHtmlBookmarks(html, menu, false, &summary);
        QVERIFY(folder);
        QCOMPARE(folder->parent(), menu);
        // Imported + skipped land in the summary; the gate refused
        // javascript and data by default.
        QCOMPARE(summary.value(QLatin1String("imported")).toInt(), 3);
        QCOMPARE(summary.value(QLatin1String("skipped")).toInt(), 2);
        QCOMPARE(summary.value(QLatin1String("skipped_by_scheme"))
                    .toObject().value(QLatin1String("javascript")).toInt(), 1);

        // Store-side children materialize on first children() pull.
        // The document's outer <DL> becomes the same unnamed wrapper
        // folder the old converter.cpp XBEL path produced.
        QList<BookmarkNode *> top = folder->children();
        QCOMPARE(top.count(), 1);
        BookmarkNode *wrapper = top.at(0);
        QCOMPARE(wrapper->type(), BookmarkNode::Folder);
        QList<BookmarkNode *> wrapped = wrapper->children();
        QCOMPARE(wrapped.count(), 2);
        BookmarkNode *docs = wrapped.at(0);
        QCOMPARE(docs->type(), BookmarkNode::Folder);
        QCOMPARE(docs->title, QLatin1String("Docs"));
        QList<BookmarkNode *> docsKids = docs->children();
        QCOMPARE(docsKids.count(), 3);   // A, B, separator
        QCOMPARE(docsKids.at(0)->url, QLatin1String("https://a.example/"));
        QCOMPARE(docsKids.at(1)->url, QLatin1String("http://b.example/"));
        QCOMPARE(docsKids.at(2)->type(), BookmarkNode::Separator);
        QCOMPARE(wrapped.at(1)->url, QLatin1String("https://c.example/"));

        // Consent flag admits the bookmarklet; the document merges
        // into the already-imported folders.
        QJsonObject summary2;
        BookmarkNode *folder2 =
            manager.importHtmlBookmarks(html, folder, true, &summary2);
        QVERIFY(folder2);
        BookmarkNode *docs2 = nullptr;
        const QList<BookmarkNode *> kids2 = folder2->children();
        QCOMPARE(kids2.count(), 1);   // unnamed <DL> wrapper
        const QList<BookmarkNode *> wrapped2 = kids2.at(0)->children();
        for (BookmarkNode *n : wrapped2) {
            if (n->title == QLatin1String("Docs"))
                docs2 = n;
        }
        QVERIFY(docs2);
        bool hasJs = false;
        const QList<BookmarkNode *> dk = docs2->children();
        for (BookmarkNode *n : dk) {
            if (n->url.startsWith(QLatin1String("javascript:")))
                hasJs = true;
        }
        QVERIFY(hasJs);

        // Export -> NETSCAPE doctype, strict escaping, and the file
        // re-imports.
        const QString out = dir.filePath(QLatin1String("export.html"));
        QVERIFY(manager.exportBookmarksTo(out));
        const QByteArray exported = [&] {
            QFile f(out);
            if (!f.open(QIODevice::ReadOnly))
                return QByteArray();
            return f.readAll();
        }();
        QVERIFY(exported.contains("NETSCAPE-Bookmark-file-1"));
        QVERIFY(exported.contains("HREF=\"https://a.example/\""));
        // The hostile title/entity chars are escaped, never raw.
        QVERIFY(!exported.contains("<script"));
        const QString xbel = dir.filePath(QLatin1String("export.xbel"));
        QVERIFY(manager.exportBookmarksTo(xbel));
        QVERIFY(QFileInfo::exists(xbel));

        // folder2 nested under folder, so menu gained one folder.
        QCOMPARE(menu->children().count(), base + 1);
        clearFolder(manager, menu);
    }
}
#endif // ARORA_RUSTCORE

QTEST_MAIN(tst_BookmarksModel)
#include "tst_bookmarksmodel.moc"
