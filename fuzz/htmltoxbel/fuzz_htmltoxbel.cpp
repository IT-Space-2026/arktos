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

// libFuzzer target for the htmlToXBel converter — the forgiving tag
// scanner that parses Netscape bookmark HTML (bookmarks import).

#include "converter.h"

#include <qbuffer.h>
#include <qguiapplication.h>

#include <stddef.h>
#include <stdint.h>

extern "C" int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    // QTextDocumentFragment (entity decoding) wants a gui app for the
    // font database; offscreen platform is set by the runner env.
    static QGuiApplication app(*argc, *argv);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const QString html =
            QString::fromUtf8(reinterpret_cast<const char *>(data),
                              qsizetype(size));
    QBuffer output;
    output.open(QIODevice::WriteOnly);
    convertHtmlToXbel(html, &output);
    return 0;
}
