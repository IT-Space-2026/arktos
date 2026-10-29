/*
 * Copyright © 2026-2027 Arktos Browser - IT-Space
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

#include "opensearchwriter.h"

#include "opensearchengine.h"

#include <qdebug.h>
#include <qiodevice.h>

/*!
    \class OpenSearchWriter
    \brief A class writing a search engine description to an external source

    OpenSearchReader is a class that can be used to write search engine descriptions
    using the OpenSearch format.

    For more information see:
    http://www.opensearch.org/Specifications/OpenSearch/1.1/Draft_4#OpenSearch_description_document

    \sa OpenSearchEngine, OpenSearchReader
*/

/*!
    Constructs a new writer.

    \note One instance can be used to write multiple search engine descriptions, one by one.
*/
OpenSearchWriter::OpenSearchWriter()
    : QXmlStreamWriter()
{
    setAutoFormatting(true);
}

/*!
    Writes an OpenSearch engine description to the \a device, filling the output document
    with all the necessary data.

    If the \a device is closed, it will be opened.

    \return true on success and false on failure.
*/
bool OpenSearchWriter::write(QIODevice *device, const OpenSearchEngine *engine)
{
    if (!engine || !device)
        return false;

    if (!device->isOpen()) {
        if (!device->open(QIODevice::WriteOnly))
            return false;
    }

    setDevice(device);
    write(engine);
    return !hasError();
}


// One <Url> element: the method/type/template attributes plus the
// optional a9 parameters namespace block — all three engine shapes
// (search, suggestions, image) write through this.
static void writeUrlElement(QXmlStreamWriter *writer,
                            const QString &method, const QString &type,
                            const QString &urlTemplate,
                            const QList<OpenSearchEngine::Parameter> &parameters,
                            const QString &purpose = QString())
{
    writer->writeStartElement(QStringLiteral("Url"));
    writer->writeAttribute(QStringLiteral("method"), method);
    writer->writeAttribute(QStringLiteral("type"), type);
    if (!purpose.isEmpty())
        writer->writeAttribute(QStringLiteral("purpose"), purpose);
    writer->writeAttribute(QStringLiteral("template"), urlTemplate);

    if (!parameters.isEmpty()) {
        writer->writeNamespace(QStringLiteral("http://a9.com/-/spec/opensearch/extensions/parameters/1.0/"), QStringLiteral("p"));

        QList<OpenSearchEngine::Parameter>::const_iterator end = parameters.constEnd();
        QList<OpenSearchEngine::Parameter>::const_iterator i = parameters.constBegin();
        for (; i != end; ++i) {
            writer->writeStartElement(QStringLiteral("p:Parameter"));
            writer->writeAttribute(QStringLiteral("name"), i->first);
            writer->writeAttribute(QStringLiteral("value"), i->second);
            writer->writeEndElement();
        }
    }

    writer->writeEndElement();
}

void OpenSearchWriter::write(const OpenSearchEngine *engine)
{
    writeStartDocument();
    writeStartElement(QStringLiteral("OpenSearchDescription"));
    writeDefaultNamespace(QStringLiteral("http://a9.com/-/spec/opensearch/1.1/"));

    if (!engine->name().isEmpty())
        writeTextElement(QStringLiteral("ShortName"), engine->name());

    if (!engine->description().isEmpty())
        writeTextElement(QStringLiteral("Description"), engine->description());

    // SENG02: <Alias> round-trips a descriptor's default keyword
    // bindings so an exported engine installs with them again.
    for (const QString &keyword : engine->defaultKeywords())
        writeTextElement(QStringLiteral("Alias"), keyword);

    if (!engine->searchUrlTemplate().isEmpty())
        writeUrlElement(this, engine->searchMethod(),
                        QStringLiteral("text/html"),
                        engine->searchUrlTemplate(),
                        engine->searchParameters());

    if (!engine->suggestionsUrlTemplate().isEmpty())
        writeUrlElement(this, engine->suggestionsMethod(),
                        QStringLiteral("application/x-suggestions+json"),
                        engine->suggestionsUrlTemplate(),
                        engine->suggestionsParameters());

    if (!engine->imageSearchUrlTemplate().isEmpty())
        writeUrlElement(this, engine->imageSearchMethod(),
                        QStringLiteral("text/html"),
                        engine->imageSearchUrlTemplate(),
                        engine->imageSearchParameters(),
                        QStringLiteral("image"));

    if (!engine->imageUrl().isEmpty())
        writeTextElement(QStringLiteral("Image"), engine->imageUrl());

    writeEndElement();
    writeEndDocument();
}

