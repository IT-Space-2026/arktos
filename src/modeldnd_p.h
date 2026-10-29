/*
 * Copyright 2026 Arktos Browser - IT-Space
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef MODELDND_P_H
#define MODELDND_P_H

#include <qaction.h>
#include <qabstractitemmodel.h>
#include <qevent.h>

#include <qdebug.h>

// CA08: the drop-position math the Qt-demo sibling widgets
// (ModelMenu, ModelToolBar) carried verbatim — an action under the
// cursor drops at its row/parent, nothing under it appends to the
// root level.
template <typename IndexFor>
void modelDropMimeData(QDropEvent *event, QAbstractItemModel *model,
                       const QModelIndex &root, QAction *action,
                       IndexFor indexFor)
{
    int row;
    QModelIndex index;
    QModelIndex parentIndex = root;
    if (!action) {
        row = model->rowCount(root);
    } else {
        index = indexFor(action);
        Q_ASSERT(index.isValid());
        row = index.row();
        if (model->hasChildren(index))
            parentIndex = index;
    }
    event->acceptProposedAction();
    model->dropMimeData(event->mimeData(), event->dropAction(),
                      row, 0, parentIndex);
}

#endif // MODELDND_P_H
