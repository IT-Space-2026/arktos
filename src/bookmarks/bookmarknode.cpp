/*
 * Copyright 2008-2014 Benjamin C. Meyer <ben@meyerhome.net>
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

#include "bookmarknode.h"

#ifdef ARORA_RUSTCORE
#include <qjsondocument.h>
#include <qjsonobject.h>

#include <rustcore.h>
#endif

BookmarkNode::BookmarkNode(BookmarkNode::Type type, BookmarkNode *parent) :
     expanded(false)
   , m_parent(parent)
   , m_type(type)
{
    if (parent)
        parent->add(this);
}

BookmarkNode::~BookmarkNode()
{
    if (m_parent)
        m_parent->remove(this);
#ifdef ARORA_RUSTCORE
    // Whatever subtree this node owns in the store — detached by the
    // remove() above or still attached at tree teardown — goes with
    // it; a fresh rc_bm_load hands out new handles anyway.
    if (m_handle) {
        rc_bm_destroy(m_handle);
        m_handle = 0;
    }
#endif
    for (int i = m_children.count() -1; i >= 0; --i)
        delete m_children[i];
    m_parent = nullptr;
    m_type = BookmarkNode::Root;
}

bool BookmarkNode::operator==(const BookmarkNode &other) const
{
    if (url != other.url
        || title != other.title
        || desc != other.desc
        || expanded != other.expanded
        || m_type != other.m_type
        || m_children.count() != other.m_children.count())
        return false;

    for (int i = 0; i < m_children.count(); ++i)
        if (!((*(m_children[i])) == (*(other.m_children[i]))))
            return false;
    return true;
}

BookmarkNode::Type BookmarkNode::type() const
{
    return m_type;
}

void BookmarkNode::setType(Type type)
{
    m_type = type;
}

QList<BookmarkNode*> BookmarkNode::children() const
{
#ifdef ARORA_RUSTCORE
    if (m_handle && !m_childrenLoaded)
        const_cast<BookmarkNode *>(this)->materializeChildren();
#endif
    return m_children;
}

BookmarkNode *BookmarkNode::parent() const
{
    return m_parent;
}

void BookmarkNode::add(BookmarkNode *child, int offset)
{
    if (!child)
        return;
    if (m_type == BookmarkNode::Bookmark)
        return;
    Q_ASSERT(child->m_type != Root);
#ifdef ARORA_RUSTCORE
    // The insert row is interpreted against the full child list, so a
    // handle-backed parent pulls its children from the store first.
    if (m_handle)
        materializeChildren();
#endif
    if (child->m_parent)
        child->m_parent->remove(child);
    child->m_parent = this;
    // Qt6 QList::insert(i > size()) extends the list with
    // uninitialized slots instead of clamping — out-of-range offsets
    // must not reach it.
    if (offset < 0 || offset > m_children.size())
        offset = m_children.size();
    m_children.insert(offset, child);
#ifdef ARORA_RUSTCORE
    if (m_handle && child->m_type != Root) {
        if (child->m_handle) {
            // Re-link a detached subtree (undo, drag-move) and resync
            // fields that may have been edited while it was out.
            if (rc_bm_attach(m_handle, offset, child->m_handle) == RC_OK)
                child->pushNodeFields();
        } else {
            child->rustUpload(m_handle, offset);
        }
    }
#endif
}

void BookmarkNode::remove(BookmarkNode *child)
{
    if (!child)
        return;
    child->m_parent = nullptr;
    m_children.removeAll(child);
#ifdef ARORA_RUSTCORE
    // detach() keeps the subtree addressable — the undo stack
    // re-links it via add() -> rc_bm_attach().
    if (child->m_handle)
        rc_bm_detach(child->m_handle);
#endif
}

#ifdef ARORA_RUSTCORE

void BookmarkNode::materializeChildren()
{
    if (m_childrenLoaded)
        return;
    m_childrenLoaded = true;
    const int64_t count = rc_bm_child_count(m_handle);
    for (int64_t i = 0; i < count; ++i) {
        const uint64_t h = rc_bm_child_at(m_handle, i);
        if (!h)
            continue;
        char *json = rc_bm_get(h);
        if (!json)
            continue;
        const QJsonObject o =
            QJsonDocument::fromJson(QByteArray(json)).object();
        rc_string_free(json);
        int kind = o.value(QLatin1String("type")).toInt(int(Folder));
        if (kind == Root || kind > Separator)
            kind = Folder;
        BookmarkNode *node = new BookmarkNode(Type(kind));
        node->m_handle = h;
        node->m_childrenLoaded = false;
        node->title = o.value(QLatin1String("title")).toString();
        node->url = o.value(QLatin1String("url")).toString();
        node->desc = o.value(QLatin1String("desc")).toString();
        node->expanded = o.value(QLatin1String("expanded")).toBool();
        node->m_parent = this;
        m_children.append(node);
    }
}

// One node's fields in the shape rc_bm_get reports; children upload
// recursively through rustUpload(), not through this JSON.
QByteArray BookmarkNode::rustJson() const
{
    QJsonObject o;
    o.insert(QLatin1String("type"), int(m_type));
    o.insert(QLatin1String("title"), title);
    o.insert(QLatin1String("url"), url);
    o.insert(QLatin1String("desc"), desc);
    o.insert(QLatin1String("expanded"), expanded);
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

// Pushes this node's field values over the store copy — called after
// attach() so edits made while the node sat detached (or between
// construction and insertion) cannot drift from the canonical tree.
void BookmarkNode::pushNodeFields() const
{
    if (!m_handle)
        return;
    rc_bm_set_title(m_handle, title.toUtf8().constData());
    rc_bm_set_url(m_handle, url.toUtf8().constData());
    rc_bm_set_desc(m_handle, desc.toUtf8().constData());
    rc_bm_set_expanded(m_handle, expanded ? 1 : 0);
}

// Recursive insert for C++-only subtrees (XBEL imports, drops): every
// node is created under the Rust parent and stamped with its handle
// so later edits address the store directly.
void BookmarkNode::rustUpload(uint64_t parent, int64_t row)
{
    const QByteArray json = rustJson();
    m_handle = rc_bm_create(parent, row, json.constData());
    if (!m_handle)
        return;
    m_childrenLoaded = true;
    for (BookmarkNode *child : m_children)
        child->rustUpload(m_handle, -1);
}

#endif // ARORA_RUSTCORE

