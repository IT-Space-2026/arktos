/*
 * Copyright 2026 Arktos Browser - IT-Space
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef STREAMINGUTILS_H
#define STREAMINGUTILS_H

#include <qdatastream.h>
#include <qiodevice.h>
#include <qlist.h>

namespace StreamingUtils {

// Reads a QDataStream-serialized container while bounding the declared
// element count by the bytes actually remaining in the stream.  Qt's
// stock operator>>(QDataStream&, QList<T>&) trusts the count prefix —
// a corrupt or hostile blob can claim billions of items and exhaust
// memory on the allocation before the stream proves it is lying.
//
// Every serialized element occupies at least a 4-byte length/count
// prefix (QString, QByteArray, nested containers, QNetworkCookie), so
// count * 4 <= bytesAvailable is a valid bound.  QStringList is
// QList<QString> and uses the same helper.
template<class T>
QDataStream &readBoundedList(QDataStream &in, QList<T> &list)
{
    list.clear();
    qint32 count = 0;
    in >> count;
    const qint64 remaining = in.device()
        ? in.device()->bytesAvailable() : qint64(0);
    if (in.status() != QDataStream::Ok || count < 0
        || qint64(count) * qint64(sizeof(qint32)) > remaining) {
        in.setStatus(QDataStream::ReadCorruptData);
        return in;
    }
    for (qint32 i = 0; i < count; ++i) {
        T value;
        in >> value;
        if (in.status() != QDataStream::Ok) {
            list.clear();
            return in;
        }
        list.append(value);
    }
    return in;
}

// CA08: the (magic, version) prologue every session-state blob
// opens with — the tab strip's and the cookie jar's restoreState
// decoded it by hand.  Reads marker + version and checks them
// against the caller's bounds; on success *versionOut (when given)
// carries the blob's version for the per-version tail reads.
// `in` must already be bound to a read device holding the blob.
inline bool readStreamHeader(QDataStream &in, qint32 magic,
                             qint32 minVersion, qint32 maxVersion,
                             qint32 *versionOut = nullptr)
{
    if (in.atEnd())
        return false;
    qint32 marker = 0;
    qint32 version = 0;
    in >> marker;
    in >> version;
    if (marker != magic || version < minVersion
        || version > maxVersion)
        return false;
    if (versionOut)
        *versionOut = version;
    return true;
}

} // namespace StreamingUtils

#endif // STREAMINGUTILS_H
