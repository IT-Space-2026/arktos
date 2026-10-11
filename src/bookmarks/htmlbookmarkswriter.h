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

#ifndef HTMLBOOKMARKSWRITER_H
#define HTMLBOOKMARKSWRITER_H

class QIODevice;
class QString;
class BookmarkNode;

// Serializes a BookmarkNode tree to the Netscape Bookmark File
// format (<!DOCTYPE NETSCAPE-Bookmark-file-1>) that every mainstream
// browser imports.  UTF-8 output; titles/urls/attributes are
// HTML-escaped.
//
// BookmarkNode carries no timestamps, so ADD_DATE / LAST_MODIFIED are
// omitted — importers default them to import time.  Descriptions are
// emitted as <DD> and the toolbar folder is marked with
// PERSONAL_TOOLBAR_FOLDER so Firefox files it on the bookmarks
// toolbar.  A collapsed tree node exports its folder with the FOLDED
// attribute.
//
// When the rust build exposes rc_bookmarks_export_html (BKIMP01) this
// class is the no-rust fallback; the export path should prefer the
// FFI writer when it is linked.
class HtmlBookmarksWriter
{
public:
    HtmlBookmarksWriter();

    bool write(const QString &fileName, const BookmarkNode *root,
               const BookmarkNode *toolbarFolder = nullptr);
    bool write(QIODevice *device, const BookmarkNode *root,
               const BookmarkNode *toolbarFolder = nullptr);

private:
    void writeNode(QString *out, const BookmarkNode *node, int level) const;
    void writeChildren(QString *out, const BookmarkNode *parent, int level) const;

    const BookmarkNode *m_toolbarFolder;
};

#endif // HTMLBOOKMARKSWRITER_H
