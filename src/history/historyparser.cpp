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
                result.entries[0].title = item.title;
            continue;
        }

        if (!result.needToSort && !result.entries.isEmpty() && lastInsertedItem < item)
            result.needToSort = true;

        result.entries.prepend(item);
        lastInsertedItem = item;
    }
    if (result.needToSort)
        std::sort(result.entries.begin(), result.entries.end());
    return result;
}

} // namespace HistoryParser
