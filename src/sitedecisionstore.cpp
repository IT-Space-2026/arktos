/*
 * Copyright 2026 Arktos Browser - IT-Space
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

#include "sitedecisionstore.h"

#include "rustcorebridge.h"

#include <arktos_core.h>

#include <qjsondocument.h>
#include <qjsonobject.h>

#include <string.h>

namespace SiteDecisionStore {

// RC08: one call's facade access.  When the shared AcCore serves, a
// profile-bound AcPolicy minted per operation re-asserts the
// profile's dir inside the core — the job rustCoreEnsureDataDirFor
// did on the legacy path.  An empty profileId pins "default": the
// persona-independent kinds (permissions, trust, http-allow,
// adblock sites) must not split across profiles (MPROF02).  When the
// bridge cannot serve (no app instance yet, a non-GUI first touch,
// ac_init failure) the legacy rc_* spelling stays, anchored by the
// same explicit dir call.
class PolicyAccess
{
public:
    explicit PolicyAccess(const QString &profileId = QString())
        : h(nullptr)
    {
        if (RustCoreBridge *bridge = RustCoreBridge::serving())
            h = bridge->policy(profileId);
        if (!h) {
            rustCoreEnsureDataDirFor(
                profileId.isEmpty() ? QLatin1String("default")
                                    : profileId);
        }
    }
    ~PolicyAccess() { ac_policy_close(h); }

    AcPolicy *h;
};

bool get(const char *kind, const QString &key, QString *value)
{
    const PolicyAccess access;
    RcBuffer out{nullptr, 0};
    const QByteArray keyUtf8 = key.toUtf8();
    const RcStatus status = access.h
        ? ac_sitedec_get(access.h, kind, keyUtf8.constData(), &out)
        : rc_sitedec_get(kind, keyUtf8.constData(), &out);
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
    const PolicyAccess access;
    const QByteArray keyUtf8 = key.toUtf8();
    const QByteArray valueUtf8 = value.toUtf8();
    return (access.h
        ? ac_sitedec_set(access.h, kind, keyUtf8.constData(),
                         valueUtf8.constData())
        : rc_sitedec_set(kind, keyUtf8.constData(),
                         valueUtf8.constData())) == RC_OK;
}

bool remove(const char *kind, const QString &key)
{
    const PolicyAccess access;
    const QByteArray keyUtf8 = key.toUtf8();
    return (access.h
        ? ac_sitedec_remove(access.h, kind, keyUtf8.constData())
        : rc_sitedec_remove(kind, keyUtf8.constData())) == RC_OK;
}

bool get(const QString &profileId, const char *kind,
         const QString &key, QString *value)
{
    const PolicyAccess access(profileId);
    RcBuffer out{nullptr, 0};
    const QByteArray keyUtf8 = key.toUtf8();
    const RcStatus status = access.h
        ? ac_sitedec_get(access.h, kind, keyUtf8.constData(), &out)
        : rc_sitedec_get(kind, keyUtf8.constData(), &out);
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

bool set(const QString &profileId, const char *kind,
         const QString &key, const QString &value)
{
    const PolicyAccess access(profileId);
    const QByteArray keyUtf8 = key.toUtf8();
    const QByteArray valueUtf8 = value.toUtf8();
    return (access.h
        ? ac_sitedec_set(access.h, kind, keyUtf8.constData(),
                         valueUtf8.constData())
        : rc_sitedec_set(kind, keyUtf8.constData(),
                         valueUtf8.constData())) == RC_OK;
}

bool remove(const QString &profileId, const char *kind,
            const QString &key)
{
    const PolicyAccess access(profileId);
    const QByteArray keyUtf8 = key.toUtf8();
    return (access.h
        ? ac_sitedec_remove(access.h, kind, keyUtf8.constData())
        : rc_sitedec_remove(kind, keyUtf8.constData())) == RC_OK;
}

bool clear(const char *kind)
{
    const PolicyAccess access;
    return (access.h ? ac_sitedec_clear(access.h, kind)
                     : rc_sitedec_clear(kind)) == RC_OK;
}

bool replace(const char *kind, const QHash<QString, QString> &entries)
{
    const PolicyAccess access;
    QJsonObject obj;
    for (auto it = entries.constBegin(); it != entries.constEnd(); ++it)
        obj.insert(it.key(), it.value());
    const QByteArray json = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    const uint8_t *data = reinterpret_cast<const uint8_t *>(json.constData());
    return (access.h ? ac_sitedec_replace(access.h, kind, data,
                                          size_t(json.size()))
                     : rc_sitedec_replace(kind, data,
                                          size_t(json.size()))) == RC_OK;
}

bool replace(const QString &profileId, const char *kind,
             const QHash<QString, QString> &entries)
{
    const PolicyAccess access(profileId);
    QJsonObject obj;
    for (auto it = entries.constBegin(); it != entries.constEnd(); ++it)
        obj.insert(it.key(), it.value());
    const QByteArray json = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    const uint8_t *data = reinterpret_cast<const uint8_t *>(json.constData());
    return (access.h ? ac_sitedec_replace(access.h, kind, data,
                                          size_t(json.size()))
                     : rc_sitedec_replace(kind, data,
                                          size_t(json.size()))) == RC_OK;
}

QHash<QString, QString> entries(const char *kind)
{
    QHash<QString, QString> map;
    const PolicyAccess access;
    char *raw = access.h ? ac_sitedec_list(access.h, kind)
                         : rc_sitedec_list(kind);
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

QHash<QString, QString> entries(const QString &profileId,
                                const char *kind)
{
    QHash<QString, QString> map;
    const PolicyAccess access(profileId);
    char *raw = access.h ? ac_sitedec_list(access.h, kind)
                         : rc_sitedec_list(kind);
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
    const PolicyAccess access;
    char *raw = access.h ? ac_sitedec_snapshot(access.h)
                         : rc_sitedec_snapshot();
    if (!raw)
        return QByteArray();
    QByteArray json = QByteArray::fromRawData(raw, int(strlen(raw)));
    json.detach();
    rc_string_free(raw);
    return json;
}

QByteArray snapshot(const QString &profileId)
{
    const PolicyAccess access(profileId);
    char *raw = access.h ? ac_sitedec_snapshot(access.h)
                         : rc_sitedec_snapshot();
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
    const PolicyAccess access;
    const QByteArray hostUtf8 = host.toUtf8();
    char *raw = access.h
        ? ac_sitedec_lookup(access.h, kind, hostUtf8.constData())
        : rc_sitedec_lookup(kind, hostUtf8.constData());
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

bool lookup(const QString &profileId, const char *kind,
            const QString &host, QString *value,
            QString *matchedKey)
{
    const PolicyAccess access(profileId);
    const QByteArray hostUtf8 = host.toUtf8();
    char *raw = access.h
        ? ac_sitedec_lookup(access.h, kind, hostUtf8.constData())
        : rc_sitedec_lookup(kind, hostUtf8.constData());
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
    const PolicyAccess access;
    return access.h ? ac_sitedec_store_present(access.h) != 0
                    : rc_sitedec_store_present() != 0;
}

bool reload()
{
    const PolicyAccess access;
    return (access.h ? ac_sitedec_reload(access.h)
                     : rc_sitedec_reload()) == RC_OK;
}

bool reset()
{
    const PolicyAccess access;
    return (access.h ? ac_sitedec_reset(access.h)
                     : rc_sitedec_reset()) == RC_OK;
}

} // namespace SiteDecisionStore
