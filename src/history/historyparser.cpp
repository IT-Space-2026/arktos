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

#include "historyparser.h"

#include <algorithm>

#include <qbuffer.h>

namespace HistoryParser {

static QString atomicString(const QString &string, QHash<QString, int> &atomicHash)
{
    QHash<QString, int>::const_iterator it = atomicHash.constFind(string);
    if (it == atomicHash.constEnd()) {
        QHash<QString, int>::iterator insertedIterator = atomicHash.insert(string, 0);
        return insertedIterator.key();
    }
    return it.key();
}

Result readEntries(QDataStream &in, QHash<QString, int> &atomicHash)
{
    Result result;
    result.needToSort = false;

    // Double check that the history file is sorted as it is read in
    HistoryEntry lastInsertedItem;
    QByteArray data;
    QDataStream stream;
    QBuffer buffer;
    QString string;
    stream.setDevice(&buffer);
    while (in.device() && !in.device()->atEnd()) {
        in >> data;
        // A corrupt file can carry a bogus block length; stop rather
        // than rescanning the rest of the stream as pseudo-entries.
        if (in.status() != QDataStream::Ok)
            break;
        buffer.close();
        buffer.setBuffer(&data);
        buffer.open(QIODevice::ReadOnly);
        quint32 ver;
        stream >> ver;
        if (ver != Version)
            continue;
        HistoryEntry item;
        stream >> string;
        item.url = atomicString(string, atomicHash);
        stream >> item.dateTime;
        stream >> string;
        item.title = atomicString(string, atomicHash);

        if (!item.dateTime.isValid())
            continue;

        if (item == lastInsertedItem) {
            if (lastInsertedItem.title.isEmpty() && !result.entries.isEmpty())
                result.entries.last().title = item.title;
            continue;
        }

        if (!result.needToSort && !result.entries.isEmpty() && lastInsertedItem < item)
            result.needToSort = true;

        result.entries.append(item);
        lastInsertedItem = item;
    }
    // The file is written oldest->newest while the list is kept
    // newest-first.  Qt6's QList is contiguous storage where prepend()
    // memmoves the whole array — per-entry prepending made parsing
    // quadratic (Qt4's QList::prepend was amortized O(1)).  Appending
    // and reversing once keeps the load linear.
    if (result.needToSort)
        std::sort(result.entries.begin(), result.entries.end());
    else
        std::reverse(result.entries.begin(), result.entries.end());
    return result;
}

} // namespace HistoryParser
