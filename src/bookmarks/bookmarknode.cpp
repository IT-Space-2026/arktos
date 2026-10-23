/*
 * Copyright © 2026-2027 Arktos Browser - IT-Space
 * Copyright 2008-2014 Arora Browser
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

#ifdef ARKTOS_RUSTCORE
#include <qjsondocument.h>
#include <qjsonobject.h>

#include <arktos_core.h>

// RC08 facade dispatch — a node minted through the facade carries
// the AcBookmarks that bound its file; every op re-asserts that
// binding inside the core before touching the process-global tree.
// A null store is the legacy path (a node minted before the bridge
// existed): the rc_* spelling over whatever file is currently bound.
static RcStatus bmDestroy(AcBookmarks *s, uint64_t node)
{
    return s ? ac_bm_destroy(s, node) : rc_bm_destroy(node);
}
static RcStatus bmAttach(AcBookmarks *s, uint64_t parent,
                         int64_t row, uint64_t node)
{
    return s ? ac_bm_attach(s, parent, row, node)
             : rc_bm_attach(parent, row, node);
}
static RcStatus bmDetach(AcBookmarks *s, uint64_t node)
{
    return s ? ac_bm_detach(s, node) : rc_bm_detach(node);
}
static int64_t bmChildCount(AcBookmarks *s, uint64_t node)
{
    return s ? ac_bm_child_count(s, node) : rc_bm_child_count(node);
}
static uint64_t bmChildAt(AcBookmarks *s, uint64_t node, int64_t row)
{
    return s ? ac_bm_child_at(s, node, row)
             : rc_bm_child_at(node, row);
}
static char *bmGet(AcBookmarks *s, uint64_t node)
{
    return s ? ac_bm_get(s, node) : rc_bm_get(node);
}
static uint64_t bmCreate(AcBookmarks *s, uint64_t parent,
                         int64_t row, const char *json)
{
    return s ? ac_bm_create(s, parent, row, json)
             : rc_bm_create(parent, row, json);
}
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
#ifdef ARKTOS_RUSTCORE
    // Whatever subtree this node owns in the store — detached by the
    // remove() above or still attached at tree teardown — goes with
    // it; a fresh load hands out new handles anyway.
    if (m_handle) {
        bmDestroy(m_store, m_handle);
        m_handle = 0;
    }
#endif
    for (int i = m_children.count() -1; i >= 0; --i)
        delete m_children[i];
    m_parent = nullptr;
    m_type = BookmarkNode::Root;
}

#ifdef ARKTOS_RUSTCORE
void BookmarkNode::dropRustHandles()
{
    m_handle = 0;
    m_store = nullptr;
    // Only recurse into children already materialized — touching the
    // lazy path would resolve dead handles against the live tree.
    if (!m_childrenLoaded)
        return;
    for (BookmarkNode *child : m_children)
        child->dropRustHandles();
}
#endif

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
#ifdef ARKTOS_RUSTCORE
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
#ifdef ARKTOS_RUSTCORE
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
#ifdef ARKTOS_RUSTCORE
    if (m_handle && child->m_type != Root) {
        child->m_store = m_store;
        if (child->m_handle) {
            // Re-link a detached subtree (undo, drag-move) and resync
            // fields that may have been edited while it was out.
            if (bmAttach(m_store, m_handle, offset, child->m_handle) == RC_OK)
                child->pushNodeFields();
        } else {
            child->rustUpload(m_store, m_handle, offset);
        }
    }
#endif
}

void BookmarkNode::remove(BookmarkNode *child)
{
    if (!child)
        return;
    child->m_parent = nullptr;
    child->removeOwner = nullptr;
    m_children.removeAll(child);
#ifdef ARKTOS_RUSTCORE
    // detach() keeps the subtree addressable — the undo stack
    // re-links it via add() -> attach.
    if (child->m_handle)
        bmDetach(child->m_store, child->m_handle);
#endif
}

#ifdef ARKTOS_RUSTCORE

void BookmarkNode::materializeChildren()
{
    if (m_childrenLoaded)
        return;
    m_childrenLoaded = true;
    const int64_t count = bmChildCount(m_store, m_handle);
    for (int64_t i = 0; i < count; ++i) {
        const uint64_t h = bmChildAt(m_store, m_handle, i);
        if (!h)
            continue;
        char *json = bmGet(m_store, h);
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
        node->m_store = m_store;
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
    if (m_store) {
        ac_bm_set_title(m_store, m_handle, title.toUtf8().constData());
        ac_bm_set_url(m_store, m_handle, url.toUtf8().constData());
        ac_bm_set_desc(m_store, m_handle, desc.toUtf8().constData());
        ac_bm_set_expanded(m_store, m_handle, expanded ? 1 : 0);
    } else {
        rc_bm_set_title(m_handle, title.toUtf8().constData());
        rc_bm_set_url(m_handle, url.toUtf8().constData());
        rc_bm_set_desc(m_handle, desc.toUtf8().constData());
        rc_bm_set_expanded(m_handle, expanded ? 1 : 0);
    }
}

// Recursive insert for C++-only subtrees (XBEL imports, drops): every
// node is created under the Rust parent and stamped with its handle
// so later edits address the store directly.
void BookmarkNode::rustUpload(AcBookmarks *store, uint64_t parent,
                              int64_t row)
{
    m_store = store;
    const QByteArray json = rustJson();
    m_handle = bmCreate(store, parent, row, json.constData());
    if (!m_handle)
        return;
    m_childrenLoaded = true;
    for (BookmarkNode *child : m_children)
        child->rustUpload(store, m_handle, -1);
}

#endif // ARKTOS_RUSTCORE

