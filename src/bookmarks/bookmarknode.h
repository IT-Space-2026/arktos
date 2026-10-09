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

#ifndef BOOKMARKNODE_H
#define BOOKMARKNODE_H

#include <qlist.h>
#include <qstringlist.h>

#ifdef ARORA_RUSTCORE
#include <stdint.h>
#include <qbytearray.h>
#endif

class BookmarkNode
{
public:
    enum Type {
        Root,
        Folder,
        Bookmark,
        Separator
    };

    BookmarkNode(Type type = Root, BookmarkNode *parent = nullptr);
    ~BookmarkNode();
    bool operator==(const BookmarkNode &other) const;

    Type type() const;
    void setType(Type type);
    QList<BookmarkNode*> children() const;
    BookmarkNode *parent() const;

    void add(BookmarkNode *child, int offset = -1);
    void remove(BookmarkNode *child);

    QString url;
    QString title;
    QString desc;
    bool expanded;

#ifdef ARORA_RUSTCORE
    // rustcore store handle (RCORE02); 0 means "not backed by the
    // Rust tree" — freshly created or imported nodes get a handle
    // when add() uploads them.  children() materializes lazily from
    // the store, one handle query per node: there is no bulk tree
    // marshal, so model work stays O(touched rows).
    uint64_t handle() const { return m_handle; }
    void setHandle(uint64_t h) { m_handle = h; m_childrenLoaded = false; }
    // False while the Rust children sit unmaterialized — the save
    // path's expanded-flag sync only walks loaded subtrees.
    bool childrenLoaded() const { return m_childrenLoaded; }
#endif

private:
#ifdef ARORA_RUSTCORE
    void materializeChildren();
    void rustUpload(uint64_t parent, int64_t row);
    void pushNodeFields() const;
    QByteArray rustJson() const;
    uint64_t m_handle = 0;
    bool m_childrenLoaded = true;
#endif
    BookmarkNode *m_parent;
    Type m_type;
    QList<BookmarkNode*> m_children;

};

#endif // BOOKMARKNODE_H
