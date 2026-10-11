/*
 * Copyright 2008 Benjamin C. Meyer <ben@meyerhome.net>
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

/****************************************************************************
**
** Copyright (C) 2008-2008 Trolltech ASA. All rights reserved.
**
** This file is part of the demonstration applications of the Qt Toolkit.
**
** This file may be used under the terms of the GNU General Public
** License versions 2.0 or 3.0 as published by the Free Software
** Foundation and appearing in the files LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file.  Alternatively you may (at
** your option) use any later version of the GNU General Public
** License if such license has been publicly approved by Trolltech ASA
** (or its successors, if any) and the KDE Free Qt Foundation. In
** addition, as a special exception, Trolltech gives you certain
** additional rights. These rights are described in the Trolltech GPL
** Exception version 1.2, which can be found at
** http://www.trolltech.com/products/qt/gplexception/ and in the file
** GPL_EXCEPTION.txt in this package.
**
** Please review the following information to ensure GNU General
** Public Licensing requirements will be met:
** http://trolltech.com/products/qt/licenses/licensing/opensource/. If
** you are unsure which license is appropriate for your use, please
** review the following information:
** http://trolltech.com/products/qt/licenses/licensing/licensingoverview
** or contact the sales department at sales@trolltech.com.
**
** In addition, as a special exception, Trolltech, as the sole
** copyright holder for Qt Designer, grants users of the Qt/Eclipse
** Integration plug-in the right for the Qt/Eclipse Integration to
** link to functionality provided by Qt Designer and its related
** libraries.
**
** This file is provided "AS IS" with NO WARRANTY OF ANY KIND,
** INCLUDING THE WARRANTIES OF DESIGN, MERCHANTABILITY AND FITNESS FOR
** A PARTICULAR PURPOSE. Trolltech reserves all rights not expressly
** granted herein.
**
** This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
** WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
**
****************************************************************************/

#include "bookmarksmanager.h"

#include "autosaver.h"
#include "bookmarknode.h"
#include "bookmarksmodel.h"
#include "browserpaths.h"
#include "converter.h"
#include "htmlbookmarkswriter.h"
#include "startupprofile.h"
#include "xbelreader.h"
#include "xbelwriter.h"

#include <qbuffer.h>
#include <qcoreapplication.h>
#include <qdatetime.h>
#include <qevent.h>
#include <qfile.h>
#include <qfiledialog.h>
#include <qfileinfo.h>
#include <qheaderview.h>
#include <qicon.h>
#include <qlocale.h>
#include <qmessagebox.h>
#include <qmimedata.h>
#include <qpointer.h>
#include <qtoolbutton.h>

#include <qdebug.h>

#ifdef ARORA_RUSTCORE
#include "rustcorebridge.h"

#include <rustcore.h>

#include <qjsondocument.h>
#include <qjsonobject.h>

// Direct field writes bypass the undo commands (and BookmarkNode's
// attach-time resync), so wherever the manager rewrites a field
// itself the store copy has to be pushed explicitly.
static void rustSyncTitle(BookmarkNode *node)
{
    if (node->handle())
        rc_bm_set_title(node->handle(), node->title.toUtf8().constData());
}

static void rustSyncUrl(BookmarkNode *node)
{
    if (node->handle())
        rc_bm_set_url(node->handle(), node->url.toUtf8().constData());
}

// `expanded` is written straight on the node by the dialog/toolbar —
// no undo command — so the store learns it here at save time.  Only
// materialized subtrees are walked; untouched ones still hold what
// the Rust parser read.
static void rustSyncExpanded(const BookmarkNode *node)
{
    if (node->handle())
        rc_bm_set_expanded(node->handle(), node->expanded ? 1 : 0);
    if (node->childrenLoaded()) {
        const QList<BookmarkNode *> children = node->children();
        for (BookmarkNode *child : children)
            rustSyncExpanded(child);
    }
}
#endif // ARORA_RUSTCORE

#define BOOKMARKBAR QT_TRANSLATE_NOOP("BookmarksManager", "Bookmarks Bar")
#define BOOKMARKMENU QT_TRANSLATE_NOOP("BookmarksManager", "Bookmarks Menu")

BookmarksManager::BookmarksManager(QObject *parent)
    : QObject(parent)
    , m_loaded(false)
    , m_saveTimer(new AutoSaver(this))
    , m_toolbar(nullptr)
    , m_menu(nullptr)
    , m_bookmarkModel(nullptr)
{
    connect(this, &BookmarksManager::entryAdded,
            m_saveTimer, &AutoSaver::changeOccurred);
    connect(this, &BookmarksManager::entryRemoved,
            m_saveTimer, &AutoSaver::changeOccurred);
    connect(this, &BookmarksManager::entryChanged,
            m_saveTimer, &AutoSaver::changeOccurred);
#ifdef ARORA_RUSTCORE
    if (QCoreApplication::instance()) {
        // Rust-side writers announce themselves through the queued
        // callback->signal bridge; the topic marks the store dirty so
        // their changes reach the on-disk XBEL like local edits do.
        connect(RustCoreBridge::instance(), &RustCoreBridge::storeChanged,
                this, [this](const QString &topic) {
            if (topic == QLatin1String("bookmarks"))
                m_saveTimer->changeOccurred();
        });
    }
#endif
}

BookmarksManager *BookmarksManager::instance()
{
    static QPointer<BookmarksManager> manager;
    if (!manager)
        manager = new BookmarksManager(qApp);
    return manager;
}

BookmarksManager::~BookmarksManager()
{
    m_saveTimer->saveIfNeccessary();
}

void BookmarksManager::changeExpanded()
{
    m_saveTimer->changeOccurred();
}

void BookmarksManager::load()
{
    if (m_loaded)
        return;
    m_loaded = true;

    StartupProfile::Scope profileScope("bookmarks xbel load");

    QString bookmarkFile = BrowserPaths::dataFilePath(QLatin1String("bookmarks.xbel"));

#ifdef ARORA_RUSTCORE
    // The Rust store owns parsing and the canonical tree; the C++
    // nodes below are a lazily materialized view over its handles.
    rustCoreEnsureDataDir();
    RcStatus st = RC_OK;
    if (QFile::exists(bookmarkFile))
        st = rc_bm_load(bookmarkFile.toUtf8().constData());
    if (st != RC_OK || !QFile::exists(bookmarkFile)) {
        // First run, or a document the safe parser refused: fall back
        // to the bundled defaults like the Qt reader did, then warn.
        bool ok = false;
        QFile defaults(QLatin1String(":defaultbookmarks.xbel"));
        if (defaults.open(QIODevice::ReadOnly)) {
            const QByteArray xbel = defaults.readAll();
            ok = rc_bm_load_mem(
                    reinterpret_cast<const uint8_t *>(xbel.constData()),
                    size_t(xbel.size())) == RC_OK;
        }
        if (!ok) {
            // Last resort: an empty root can never fail to parse.
            static const char kEmptyXbel[] = "<xbel version=\"1.0\"/>";
            rc_bm_load_mem(reinterpret_cast<const uint8_t *>(kEmptyXbel),
                           sizeof(kEmptyXbel) - 1);
        }
        if (st != RC_OK) {
            char *err = rc_last_error_message();
            QMessageBox::warning(nullptr, QLatin1String("Loading Bookmark"),
                tr("Error when loading bookmarks: %1")
                    .arg(QString::fromUtf8(err ? err : "")));
            rc_string_free(err);
        }
    }
    m_bookmarkRootNode.reset(new BookmarkNode(BookmarkNode::Root));
    m_bookmarkRootNode->setHandle(rc_bm_root());
#else
    if (!QFile::exists(bookmarkFile))
        bookmarkFile = QLatin1String(":defaultbookmarks.xbel");

    XbelReader reader;
    m_bookmarkRootNode.reset(reader.read(bookmarkFile));
    if (reader.error() != QXmlStreamReader::NoError) {
        QMessageBox::warning(nullptr, QLatin1String("Loading Bookmark"),
            tr("Error when loading bookmarks on line %1, column %2:\n"
               "%3").arg(reader.lineNumber()).arg(reader.columnNumber()).arg(reader.errorString()));
    }
#endif

    QList<BookmarkNode*> others;
    for (int i = m_bookmarkRootNode->children().count() - 1; i >= 0; --i) {
        BookmarkNode *node = m_bookmarkRootNode->children().at(i);
        if (node->type() == BookmarkNode::Folder) {
            // Automatically convert
            if ((node->title == tr("Toolbar Bookmarks")
                 || node->title == QLatin1String(BOOKMARKBAR)) && !m_toolbar) {
                node->title = tr(BOOKMARKBAR);
#ifdef ARORA_RUSTCORE
                rustSyncTitle(node);
#endif

                m_toolbar = node;
            }

            // Automatically convert
            if ((node->title == tr("Menu")
                 || node->title == QLatin1String(BOOKMARKMENU)) && !m_menu) {
                node->title = tr(BOOKMARKMENU);
#ifdef ARORA_RUSTCORE
                rustSyncTitle(node);
#endif
                m_menu = node;
            }
        } else {
            others.append(node);
        }
        m_bookmarkRootNode->remove(node);
    }
    Q_ASSERT(m_bookmarkRootNode->children().count() == 0);
    if (!m_toolbar) {
        m_toolbar = new BookmarkNode(BookmarkNode::Folder, m_bookmarkRootNode.get());
        m_toolbar->title = tr(BOOKMARKBAR);
#ifdef ARORA_RUSTCORE
        rustSyncTitle(m_toolbar);
#endif
    } else {
        m_bookmarkRootNode->add(m_toolbar);
    }

    if (!m_menu) {
        m_menu = new BookmarkNode(BookmarkNode::Folder, m_bookmarkRootNode.get());
        m_menu->title = tr(BOOKMARKMENU);
#ifdef ARORA_RUSTCORE
        rustSyncTitle(m_menu);
#endif
    } else {
        m_bookmarkRootNode->add(m_menu);
    }

    for (int i = 0; i < others.count(); ++i)
        m_menu->add(others.at(i));
}

void BookmarksManager::save() const
{
    if (!m_loaded)
        return;

    QString bookmarkFile = BrowserPaths::dataFilePath(QLatin1String("bookmarks.xbel"));
    // Save root folder titles in English (i.e. not localized)
    m_menu->title = QLatin1String(BOOKMARKMENU);
    m_toolbar->title = QLatin1String(BOOKMARKBAR);
#ifdef ARORA_RUSTCORE
    rustCoreEnsureDataDir();
    rustSyncTitle(m_menu);
    rustSyncTitle(m_toolbar);
    rustSyncExpanded(m_bookmarkRootNode.get());
    // The store serializes and installs the file atomically itself.
    if (rc_bm_save(bookmarkFile.toUtf8().constData()) != RC_OK)
        qWarning() << "BookmarkManager: error saving to" << bookmarkFile;
#else
    XbelWriter writer;
    if (!writer.write(bookmarkFile, m_bookmarkRootNode.get()))
        qWarning() << "BookmarkManager: error saving to" << bookmarkFile;
#endif
    // Restore localized titles
    retranslate();
}

void BookmarksManager::retranslate() const
{
    if (m_menu) {
        m_menu->title = tr(BOOKMARKMENU);
#ifdef ARORA_RUSTCORE
        rustSyncTitle(m_menu);
#endif
    }
    if (m_toolbar) {
        m_toolbar->title = tr(BOOKMARKBAR);
#ifdef ARORA_RUSTCORE
        rustSyncTitle(m_toolbar);
#endif
    }
}

void BookmarksManager::addBookmark(BookmarkNode *parent, BookmarkNode *node, int row)
{
    if (!m_loaded)
        return;
    Q_ASSERT(parent);
    InsertBookmarksCommand *command = new InsertBookmarksCommand(this, parent, node, row);
    m_commands.push(command);
}

void BookmarksManager::removeBookmark(BookmarkNode *node)
{
    if (!m_loaded)
        return;

    Q_ASSERT(node);
    BookmarkNode *parent = node->parent();
    int row = parent->children().indexOf(node);
    RemoveBookmarksCommand *command = new RemoveBookmarksCommand(this, parent, row);
    m_commands.push(command);
}

void BookmarksManager::setTitle(BookmarkNode *node, const QString &newTitle)
{
    if (!m_loaded)
        return;

    Q_ASSERT(node);
    ChangeBookmarkCommand *command = new ChangeBookmarkCommand(this, node, newTitle, true);
    m_commands.push(command);
}

void BookmarksManager::setUrl(BookmarkNode *node, const QString &newUrl)
{
    if (!m_loaded)
        return;

    Q_ASSERT(node);
    ChangeBookmarkCommand *command = new ChangeBookmarkCommand(this, node, newUrl, false);
    m_commands.push(command);
}

BookmarkNode *BookmarksManager::bookmarks()
{
    if (!m_loaded)
        load();
    return m_bookmarkRootNode.get();
}

BookmarkNode *BookmarksManager::menu()
{
    if (!m_loaded)
        load();

    Q_ASSERT(m_menu);
    return m_menu;
}

BookmarkNode *BookmarksManager::toolbar()
{
    if (!m_loaded)
        load();

    Q_ASSERT(m_toolbar);
    return m_toolbar;
}

BookmarksModel *BookmarksManager::bookmarksModel()
{
    if (!m_bookmarkModel)
        m_bookmarkModel = new BookmarksModel(this, this);
    return m_bookmarkModel;
}

void BookmarksManager::importBookmarks()
{
    QStringList supportedFormats;
    supportedFormats << tr("XBEL bookmarks").append(QLatin1String("(*.xbel *.xml)"));
    supportedFormats << tr("HTML Netscape bookmarks").append(QLatin1String("(*.html)"));

    QString fileName = QFileDialog::getOpenFileName(nullptr, tr("Open File"),
                                                    QString(), supportedFormats.join(QLatin1String(";;")));
    if (fileName.isEmpty())
        return;

    XbelReader reader;
    BookmarkNode *importRootNode = nullptr;
    if (fileName.endsWith(QLatin1String(".html"))) {
        // Convert in-process: the Netscape-HTML parser is shared with
        // tools/htmlToXBel and is internally bounded (256 KiB
        // closing-tag scan windows), so it is safe on the GUI thread.
        // The Qt4 flow spawned the tool and blocked the event loop in
        // waitForFinished(-1) — and the binary is never installed, so
        // the feature was dead in packaged builds anyway.
        QFile file(fileName);
        if (!file.open(QIODevice::ReadOnly)) {
            QMessageBox::warning(nullptr, tr("Loading Bookmark"),
                tr("Unable to open HTML bookmarks file %1.").arg(fileName));
            return;
        }
        if (file.size() > 64 * 1024 * 1024) {
            QMessageBox::warning(nullptr, tr("Loading Bookmark"),
                tr("Bookmarks file %1 is too large to import.").arg(fileName));
            return;
        }
        const QByteArray html = file.readAll();
#ifdef ARORA_RUSTCORE
        // BKIMP01: parse + insert in rustcore — bounded scan, merge/
        // dedup, and the url-scheme gate at the store boundary.
        importHtmlDocument(html);
        return;
#else
        // Convert in-process: the Netscape-HTML parser is shared with
        // tools/htmlToXBel and is internally bounded (256 KiB
        // closing-tag scan windows), so it is safe on the GUI thread.
        // The Qt4 flow spawned the tool and blocked the event loop in
        // waitForFinished(-1) — and the binary is never installed, so
        // the feature was dead in packaged builds anyway.
        QBuffer buffer;
        buffer.open(QIODevice::ReadWrite);
        if (convertHtmlToXbel(QString::fromUtf8(html), &buffer) != 0) {
            QMessageBox::warning(nullptr, tr("Loading Bookmark"),
                tr("Error when loading HTML bookmarks: no bookmarks "
                   "found in %1.").arg(fileName));
            return;
        }
        buffer.seek(0);
        importRootNode = reader.read(&buffer);
#endif
    } else {
        importRootNode = reader.read(fileName);
    }
    if (reader.error() != QXmlStreamReader::NoError) {
        QMessageBox::warning(nullptr, QLatin1String("Loading Bookmark"),
            tr("Error when loading bookmarks on line %1, column %2:\n"
               "%3").arg(reader.lineNumber()).arg(reader.columnNumber()).arg(reader.errorString()));
        delete importRootNode;
        return;
    }

    importRootNode->setType(BookmarkNode::Folder);
    importRootNode->title = (tr("Imported %1").arg(QLocale().toString(QDate::currentDate(), QLocale::ShortFormat)));
    addBookmark(menu(), importRootNode);
}

void BookmarksManager::exportBookmarks()
{
#ifdef ARORA_RUSTCORE
    QStringList supportedFormats;
    supportedFormats << tr("XBEL bookmarks").append(QLatin1String("(*.xbel *.xml)"));
    supportedFormats << tr("HTML Netscape bookmarks").append(QLatin1String("(*.html)"));
    QString selectedFilter = supportedFormats.first();
    QString fileName = QFileDialog::getSaveFileName(nullptr, tr("Save File"),
                                tr("%1 Bookmarks.xbel").arg(QCoreApplication::applicationName()),
                                supportedFormats.join(QLatin1String(";;")),
                                &selectedFilter);
    if (fileName.isEmpty())
        return;
    // The file dialog doesn't add the selected-format suffix on its
    // own — a "Netscape" choice without .html writes XBEL otherwise.
    if (selectedFilter.contains(QLatin1String("HTML"))
            && !fileName.endsWith(QLatin1String(".html"), Qt::CaseInsensitive))
        fileName.append(QLatin1String(".html"));

    if (!exportBookmarksTo(fileName))
        QMessageBox::critical(nullptr, tr("Export error"), tr("error saving bookmarks"));
#else
    const QString xbelFilter = tr("XBEL bookmarks").append(QLatin1String("(*.xbel *.xml)"));
    const QString htmlFilter = tr("Netscape bookmarks").append(QLatin1String("(*.html)"));
    QString selectedFilter = xbelFilter;
    QString fileName = QFileDialog::getSaveFileName(nullptr, tr("Save File"),
                                tr("%1 Bookmarks.xbel").arg(QCoreApplication::applicationName()),
                                xbelFilter + QLatin1String(";;") + htmlFilter,
                                &selectedFilter);
    if (fileName.isEmpty())
        return;

    // A typed known extension wins over the selected filter; otherwise
    // the filter decides and the matching suffix is appended.
    const QString suffix = QFileInfo(fileName).suffix().toLower();
    bool html;
    if (suffix == QLatin1String("html") || suffix == QLatin1String("htm"))
        html = true;
    else if (suffix == QLatin1String("xbel") || suffix == QLatin1String("xml"))
        html = false;
    else {
        html = (selectedFilter == htmlFilter);
        fileName += html ? QLatin1String(".html") : QLatin1String(".xbel");
    }

    bool saved;
    if (html) {
        HtmlBookmarksWriter writer;
        saved = writer.write(fileName, bookmarks(), toolbar());
    } else {
        XbelWriter writer;
        saved = writer.write(fileName, bookmarks());
    }
    if (!saved)
        QMessageBox::critical(nullptr, tr("Export error"), tr("error saving bookmarks"));
#endif
}

#ifdef ARORA_RUSTCORE
// BKIMP01: the rust-side import driver.  A dry run first counts what
// the document would insert (and what the url-scheme gate would
// refuse) without touching the store, so the bookmarklet question is
// settled before the first insert.
void BookmarksManager::importHtmlDocument(const QByteArray &html)
{
    if (!m_loaded)
        load();
    const uint64_t rootHandle = menu()->handle();
    char *probeJson = rc_bm_import_html(rootHandle,
            reinterpret_cast<const uint8_t *>(html.constData()),
            size_t(html.size()), RC_BM_IMPORT_DRY_RUN);
    if (!probeJson) {
        char *err = rc_last_error_message();
        QMessageBox::warning(nullptr, tr("Loading Bookmark"),
            tr("Error when loading HTML bookmarks: %1")
                .arg(QString::fromUtf8(err ? err : "")));
        rc_string_free(err);
        return;
    }
    const QJsonObject probe =
        QJsonDocument::fromJson(QByteArray(probeJson)).object();
    rc_string_free(probeJson);
    int items = 0;
    for (const char *key : {"imported", "folders", "separators",
                            "duplicates", "skipped"})
        items += probe.value(QLatin1String(key)).toInt();
    if (items == 0) {
        QMessageBox::warning(nullptr, tr("Loading Bookmark"),
            tr("Error when loading HTML bookmarks: no bookmarks "
               "found."));
        return;
    }
    const int bookmarklets =
        probe.value(QLatin1String("skipped_by_scheme")).toObject()
            .value(QLatin1String("javascript")).toInt();
    bool importBookmarklets = false;
    if (bookmarklets > 0) {
        importBookmarklets = QMessageBox::question(nullptr,
            tr("Import bookmarklets?"),
            tr("The file contains %1 bookmarklet(s) — javascript: links "
               "that run script in the current page when opened.\n"
               "Import them anyway?").arg(bookmarklets),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
            == QMessageBox::Yes;
    }
    QJsonObject summary;
    if (!importHtmlBookmarks(html, menu(), importBookmarklets, &summary)) {
        QMessageBox::warning(nullptr, tr("Loading Bookmark"),
            tr("Error when loading HTML bookmarks: no importable "
               "bookmarks found."));
        return;
    }
    const int imported = summary.value(QLatin1String("imported")).toInt();
    const int skipped = summary.value(QLatin1String("skipped")).toInt();
    QMessageBox::information(nullptr, tr("Bookmarks imported"),
        skipped > 0
            ? tr("%1 bookmark item(s) imported; %2 item(s) were skipped "
                 "as unsafe or unsupported URLs.")
                  .arg(imported).arg(skipped)
            : tr("%1 bookmark item(s) imported.").arg(imported));
}

BookmarkNode *BookmarksManager::importHtmlBookmarks(const QByteArray &html,
        BookmarkNode *parent, bool importBookmarklets, QJsonObject *summaryOut)
{
    if (!m_loaded)
        load();
    if (!parent)
        parent = menu();
    BookmarkNode *folder = new BookmarkNode(BookmarkNode::Folder);
    folder->title = (tr("Imported %1")
            .arg(QLocale().toString(QDate::currentDate(), QLocale::ShortFormat)));
    addBookmark(parent, folder);
    char *json = nullptr;
    if (folder->handle()) {
        json = rc_bm_import_html(folder->handle(),
                reinterpret_cast<const uint8_t *>(html.constData()),
                size_t(html.size()),
                importBookmarklets ? RC_BM_IMPORT_BOOKMARKLETS : 0);
    }
    QJsonObject summary;
    if (json) {
        summary = QJsonDocument::fromJson(QByteArray(json)).object();
        rc_string_free(json);
    }
    const int items = summary.value(QLatin1String("imported")).toInt()
                    + summary.value(QLatin1String("folders")).toInt()
                    + summary.value(QLatin1String("separators")).toInt();
    if (items == 0) {
        // Nothing landed (parse failure or a fully-skipped document):
        // drop the empty folder so the user sees no trace.
        removeBookmark(folder);
        return nullptr;
    }
    // The store-side children were inserted behind this proxy's back.
    folder->invalidateChildren();
    if (summaryOut)
        *summaryOut = summary;
    return folder;
}

bool BookmarksManager::exportBookmarksTo(const QString &fileName)
{
    if (!m_loaded)
        load();
    // .html is the BKIMP01 Netscape writer; anything else is the
    // canonical XBEL — both atomic on the rust side.
    if (fileName.endsWith(QLatin1String(".html"), Qt::CaseInsensitive))
        return rc_bm_export_html(fileName.toUtf8().constData()) == RC_OK;
    return rc_bm_save(fileName.toUtf8().constData()) == RC_OK;
}
#endif // ARORA_RUSTCORE

RemoveBookmarksCommand::RemoveBookmarksCommand(BookmarksManager *m_bookmarkManagaer, BookmarkNode *parent, int row)
    : QUndoCommand(BookmarksManager::tr("Remove Bookmark"))
    , m_row(row)
    , m_bookmarkManagaer(m_bookmarkManagaer)
    , m_node(parent->children().value(row))
    , m_parent(parent)
    , m_done(false)
{
}

RemoveBookmarksCommand::~RemoveBookmarksCommand()
{
    if (m_done && !m_node->parent()) {
        delete m_node;
    }
}

void RemoveBookmarksCommand::undo()
{
    m_parent->add(m_node, m_row);
    emit m_bookmarkManagaer->entryAdded(m_node);
    m_done = false;
}

void RemoveBookmarksCommand::redo()
{
    m_parent->remove(m_node);
    emit m_bookmarkManagaer->entryRemoved(m_parent, m_row, m_node);
    m_done = true;
}

InsertBookmarksCommand::InsertBookmarksCommand(BookmarksManager *m_bookmarkManagaer,
                BookmarkNode *parent, BookmarkNode *node, int row)
    : RemoveBookmarksCommand(m_bookmarkManagaer, parent, row)
{
    setText(BookmarksManager::tr("Insert Bookmark"));
    m_node = node;
}

ChangeBookmarkCommand::ChangeBookmarkCommand(BookmarksManager *m_bookmarkManagaer, BookmarkNode *node,
                        const QString &newValue, bool title)
    : QUndoCommand()
    , m_bookmarkManagaer(m_bookmarkManagaer)
    , m_title(title)
    , m_newValue(newValue)
    , m_node(node)
{
    if (m_title) {
        m_oldValue = m_node->title;
        setText(BookmarksManager::tr("Name Change", "Undo bookmark title change"));
    } else {
        m_oldValue = m_node->url;
        setText(BookmarksManager::tr("Address Change", "Undo bookmark url change"));
    }
}

void ChangeBookmarkCommand::undo()
{
    if (m_title) {
        m_node->title = m_oldValue;
#ifdef ARORA_RUSTCORE
        rustSyncTitle(m_node);
#endif
    } else {
        m_node->url = m_oldValue;
#ifdef ARORA_RUSTCORE
        rustSyncUrl(m_node);
#endif
    }
    emit m_bookmarkManagaer->entryChanged(m_node);
}

void ChangeBookmarkCommand::redo()
{
    if (m_title) {
        m_node->title = m_newValue;
#ifdef ARORA_RUSTCORE
        rustSyncTitle(m_node);
#endif
    } else {
        m_node->url = m_newValue;
#ifdef ARORA_RUSTCORE
        rustSyncUrl(m_node);
#endif
    }
    emit m_bookmarkManagaer->entryChanged(m_node);
}

