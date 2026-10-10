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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

#include "sitedecisionstore.h"

#include "rustcore.h"
#include "rustcorebridge.h"

#include <qjsondocument.h>
#include <qjsonobject.h>

namespace SiteDecisionStore {

static void ensureDir()
{
    // Cheap and idempotent — the Qt shim calls it before every op so
    // test-mode path switches keep working.
    rustCoreEnsureDataDir();
}

bool get(const char *kind, const QString &key, QString *value)
{
    ensureDir();
    RcBuffer out{nullptr, 0};
    const QByteArray keyUtf8 = key.toUtf8();
    const RcStatus status =
        rc_sitedec_get(kind, keyUtf8.constData(), &out);
    if (status != RC_OK || !out.data) {
        if (out.data)
            rc_buffer_free(out);
        return false;
    }
    if (value)
        *value = QString::fromUtf8(reinterpret_cast<const char *>(out.data),
                                   int(out.len));
    rc_buffer_free(out);
    return true;
}

bool set(const char *kind, const QString &key, const QString &value)
{
    ensureDir();
    const QByteArray keyUtf8 = key.toUtf8();
    const QByteArray valueUtf8 = value.toUtf8();
    return rc_sitedec_set(kind, keyUtf8.constData(), valueUtf8.constData())
        == RC_OK;
}

bool remove(const char *kind, const QString &key)
{
    ensureDir();
    const QByteArray keyUtf8 = key.toUtf8();
    return rc_sitedec_remove(kind, keyUtf8.constData()) == RC_OK;
}

bool clear(const char *kind)
{
    ensureDir();
    return rc_sitedec_clear(kind) == RC_OK;
}

bool replace(const char *kind, const QHash<QString, QString> &entries)
{
    ensureDir();
    QJsonObject obj;
    for (auto it = entries.constBegin(); it != entries.constEnd(); ++it)
        obj.insert(it.key(), it.value());
    const QByteArray json = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    return rc_sitedec_replace(
               kind, reinterpret_cast<const uint8_t *>(json.constData()),
               size_t(json.size()))
        == RC_OK;
}

QHash<QString, QString> entries(const char *kind)
{
    QHash<QString, QString> map;
    ensureDir();
    char *raw = rc_sitedec_list(kind);
    if (!raw)
        return map;
    const QByteArray json = QByteArray::fromRawData(raw, int(strlen(raw)));
    const QJsonObject obj =
        QJsonDocument::fromJson(json).object();
    rc_string_free(raw);
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
        if (it.value().isString())
            map.insert(it.key(), it.value().toString());
    }
    return map;
}

QByteArray snapshot()
{
    ensureDir();
    char *raw = rc_sitedec_snapshot();
    if (!raw)
        return QByteArray();
    QByteArray json = QByteArray::fromRawData(raw, int(strlen(raw)));
    json.detach();
    rc_string_free(raw);
    return json;
}

bool lookup(const char *kind, const QString &host, QString *value,
            QString *matchedKey)
{
    ensureDir();
    const QByteArray hostUtf8 = host.toUtf8();
    char *raw = rc_sitedec_lookup(kind, hostUtf8.constData());
    if (!raw)
        return false;
    const QJsonObject obj = QJsonDocument::fromJson(
        QByteArray::fromRawData(raw, int(strlen(raw)))).object();
    rc_string_free(raw);
    const QString v = obj.value(QLatin1String("value")).toString();
    if (v.isEmpty())
        return false;
    if (value)
        *value = v;
    if (matchedKey)
        *matchedKey = obj.value(QLatin1String("key")).toString();
    return true;
}

bool storePresent()
{
    ensureDir();
    return rc_sitedec_store_present() != 0;
}

bool reload()
{
    ensureDir();
    return rc_sitedec_reload() == RC_OK;
}

bool reset()
{
    ensureDir();
    return rc_sitedec_reset() == RC_OK;
}

} // namespace SiteDecisionStore
