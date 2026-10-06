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

#ifndef HISTORYPARSER_H
#define HISTORYPARSER_H

#include "historymanager.h"

#include <qdatastream.h>
#include <qhash.h>
#include <qlist.h>

// Decoding of the on-disk history format, extracted from
// HistoryManager::load() so the parser can be exercised directly by
// autotests and the fuzz targets without instantiating the manager.
namespace HistoryParser {

// Stream format version tag written as the first field of every
// per-entry block.
static const unsigned int Version = 23;

struct Result {
    QList<HistoryEntry> entries;   // newest first
    bool needToSort;               // input was not already sorted
};

// Reads the sequence of QByteArray blocks written by
// HistoryManager::save() — each block is a Version-tagged
// url/dateTime/title tuple.  Interned strings are shared through
// atomicHash (same dedup as HistoryManager::atomicString).  Entries
// identical to their predecessor collapse into the previous entry;
// entries with an invalid timestamp are skipped.  The scan stops at
// the first corrupt block rather than rescanning the rest of the
// stream as pseudo-entries.
Result readEntries(QDataStream &in, QHash<QString, int> &atomicHash);

} // namespace HistoryParser

#endif // HISTORYPARSER_H
