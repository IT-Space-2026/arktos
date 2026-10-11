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

#include "htmlbookmarkswriter.h"

#include <qfile.h>

#include "bookmarknode.h"

HtmlBookmarksWriter::HtmlBookmarksWriter()
    : m_toolbarFolder(nullptr)
{
}

bool HtmlBookmarksWriter::write(const QString &fileName, const BookmarkNode *root,
                                const BookmarkNode *toolbarFolder)
{
    QFile file(fileName);
    if (!root || !file.open(QFile::WriteOnly))
        return false;
    return write(&file, root, toolbarFolder);
}

bool HtmlBookmarksWriter::write(QIODevice *device, const BookmarkNode *root,
                                const BookmarkNode *toolbarFolder)
{
    if (!device || !root || !device->isWritable())
        return false;

    m_toolbarFolder = toolbarFolder;

    QString html;
    html += QLatin1String("<!DOCTYPE NETSCAPE-Bookmark-file-1>\n"
            "<!-- This is an automatically generated file.\n"
            "     It will be read and overwritten.\n"
            "     DO NOT EDIT! -->\n"
            "<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=UTF-8\">\n"
            "<TITLE>Bookmarks</TITLE>\n"
            "<H1>Bookmarks</H1>\n"
            "<DL><p>\n");
    if (root->type() == BookmarkNode::Root)
        writeChildren(&html, root, 1);
    else
        writeNode(&html, root, 1);
    html += QLatin1String("</DL><p>\n");

    return device->write(html.toUtf8()) >= 0;
}

void HtmlBookmarksWriter::writeChildren(QString *out, const BookmarkNode *parent, int level) const
{
    const QList<BookmarkNode *> children = parent->children();
    for (BookmarkNode *child : children)
        writeNode(out, child, level);
}

void HtmlBookmarksWriter::writeNode(QString *out, const BookmarkNode *node, int level) const
{
    const QString indent(4 * level, QLatin1Char(' '));
    switch (node->type()) {
    case BookmarkNode::Folder: {
        *out += indent + QLatin1String("<DT><H3");
        if (!node->expanded)
            *out += QLatin1String(" FOLDED");
        if (node == m_toolbarFolder)
            *out += QLatin1String(" PERSONAL_TOOLBAR_FOLDER=\"true\"");
        *out += QLatin1Char('>') + node->title.toHtmlEscaped()
                + QLatin1String("</H3>\n");
        *out += indent + QLatin1String("<DL><p>\n");
        writeChildren(out, node, level + 1);
        *out += indent + QLatin1String("</DL><p>\n");
        break;
    }
    case BookmarkNode::Bookmark:
        *out += indent + QLatin1String("<DT><A HREF=\"")
                + node->url.toHtmlEscaped() + QLatin1String("\">")
                + node->title.toHtmlEscaped() + QLatin1String("</A>\n");
        if (!node->desc.isEmpty())
            *out += indent + QLatin1String("<DD>")
                    + node->desc.toHtmlEscaped() + QLatin1Char('\n');
        break;
    case BookmarkNode::Separator:
        *out += indent + QLatin1String("<HR>\n");
        break;
    default:
        break;
    }
}
