/*
 * Copyright 2009 Arora Browser
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

#ifndef OPENSEARCHWRITER_H
#define OPENSEARCHWRITER_H

#include <qxmlstream.h>

class OpenSearchEngine;

class OpenSearchWriter : public QXmlStreamWriter
{
public:
    OpenSearchWriter();

    // Opens \a device for writing when it is closed; \a engine is only
    // read from.  Returns false when the device cannot be opened or
    // \a engine is null.
    [[nodiscard]] bool write(QIODevice *device, const OpenSearchEngine *engine);

private:
    void write(const OpenSearchEngine *engine);
};

#endif // OPENSEARCHWRITER_H

