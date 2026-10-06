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

// libFuzzer target for StreamingUtils::readBoundedList — the SEC04
// guard that bounds QDataStream container counts by remaining bytes.
// Exercises it on the element types used by the app stores (autofill
// forms, tab sessions, cookie trie) plus a Form-shaped composite
// sequence.

#include "streamingutils.h"

// QNetworkCookie's QDataStream operators live in the jar's private
// header (cookies serialize via toRawForm(), not a Qt operator).
#include "networkcookiejar_p.h"

#include <qbuffer.h>
#include <qnetworkcookie.h>

#include <stddef.h>
#include <stdint.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    QByteArray bytes(reinterpret_cast<const char *>(data), qsizetype(size));

    {
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::ReadOnly);
        QDataStream in(&buffer);
        QList<QString> list;
        StreamingUtils::readBoundedList(in, list);
    }
    {
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::ReadOnly);
        QDataStream in(&buffer);
        QList<QByteArray> list;
        StreamingUtils::readBoundedList(in, list);
    }
    {
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::ReadOnly);
        QDataStream in(&buffer);
        QList<QNetworkCookie> list;
        StreamingUtils::readBoundedList(in, list);
    }
    {
        // AutoFillManager::Form::load layout: element list + url +
        // name + flag.
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::ReadOnly);
        QDataStream in(&buffer);
        QStringList elements;
        QString url;
        QString name;
        bool flag;
        StreamingUtils::readBoundedList(in, elements);
        in >> url >> name >> flag;
    }
    return 0;
}
