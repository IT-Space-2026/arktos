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

// libFuzzer target for the on-disk history file format — a sequence of
// QByteArray blocks, each a version-tagged url/dateTime/title tuple.
// Exercises the real decode path shared with HistoryManager::load()
// (src/history/historyparser.cpp).

#include "historyparser.h"

#include <qbuffer.h>

#include <stddef.h>
#include <stdint.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    QByteArray bytes(reinterpret_cast<const char *>(data), qsizetype(size));
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::ReadOnly);
    QDataStream in(&buffer);

    QHash<QString, int> atomicHash;
    const HistoryParser::Result result =
            HistoryParser::readEntries(in, atomicHash);
    for (const HistoryEntry &entry : result.entries) {
        (void)entry.url;
        (void)entry.title;
        (void)entry.dateTime.isValid();
    }
    return 0;
}
