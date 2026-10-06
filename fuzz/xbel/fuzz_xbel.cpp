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

// libFuzzer target for the XBEL bookmark reader (bookmarks import,
// default bookmarks).  Parses the input, walks the tree, then
// round-trips it through XbelWriter and reads the result back.

#include "bookmarknode.h"
#include "xbelreader.h"
#include "xbelwriter.h"

#include <qbuffer.h>

#include <stddef.h>
#include <stdint.h>

static void walk(const BookmarkNode *node, int depth)
{
    // The reader bounds element nesting; keep the walk bounded anyway
    // so a corrupted tree can't recurse unbounded here.
    if (!node || depth <= 0)
        return;
    node->type();
    node->url.size();
    node->title.size();
    node->desc.size();
    const QList<BookmarkNode *> children = node->children();
    for (const BookmarkNode *child : children)
        walk(child, depth - 1);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    QByteArray bytes(reinterpret_cast<const char *>(data), qsizetype(size));
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::ReadOnly);

    XbelReader reader;
    BookmarkNode *root = reader.read(&buffer);
    if (!root)
        return 0;

    walk(root, 512);

    QByteArray written;
    QBuffer outBuffer(&written);
    outBuffer.open(QIODevice::WriteOnly);
    XbelWriter writer;
    writer.write(&outBuffer, root);
    delete root;

    if (!written.isEmpty()) {
        QBuffer reBuffer(&written);
        reBuffer.open(QIODevice::ReadOnly);
        XbelReader reReader;
        BookmarkNode *reparsed = reReader.read(&reBuffer);
        delete reparsed;
    }
    return 0;
}
