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

// libFuzzer target for the OpenSearch description parser — the XML a
// search-engine install feed or <link rel=search> document hands us.
// Exercises read(), the resulting engine's accessors, and a writer
// round-trip.

#include "opensearchengine.h"
#include "opensearchreader.h"
#include "opensearchwriter.h"

#include <qbuffer.h>

#include <stddef.h>
#include <stdint.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    QByteArray bytes(reinterpret_cast<const char *>(data), qsizetype(size));
    QBuffer buffer(&bytes);

    OpenSearchReader reader;
    OpenSearchEngine *engine = reader.read(&buffer);
    if (!engine)
        return 0;

    engine->name();
    engine->description();
    engine->searchUrlTemplate();
    engine->suggestionsUrlTemplate();
    engine->imageUrl();
    engine->searchParameters();
    engine->suggestionsParameters();
    engine->providesSuggestions();

    // Serialize whatever survived the parse and feed it back in.
    QByteArray written;
    QBuffer outBuffer(&written);
    outBuffer.open(QIODevice::WriteOnly);
    OpenSearchWriter writer;
    writer.write(&outBuffer, engine);
    delete engine;

    if (!written.isEmpty()) {
        QBuffer reBuffer(&written);
        OpenSearchReader reReader;
        OpenSearchEngine *reparsed = reReader.read(&reBuffer);
        delete reparsed;
    }
    return 0;
}
