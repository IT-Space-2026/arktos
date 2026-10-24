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

// MB07: every op is one envelope round trip through ac_mw_dispatch
// when the bridge serves — the payload's "profile" binds
// profiles/<id>/ per call, so no AcPolicy mints.  An empty profileId
// pins "default": the persona-independent kinds (permissions, trust,
// http-allow, adblock sites) must not split across profiles
// (MPROF02).  When the bridge cannot serve (no app instance yet, a
// non-GUI first touch, ac_init failure) the legacy rc_* spelling
// stays, anchored by the same explicit dir call.
static RustCoreBridge *policyAccess(const QString &profileId)
{
    if (RustCoreBridge *bridge = RustCoreBridge::serving())
        return bridge;
    rustCoreEnsureDataDirFor(
        profileId.isEmpty() ? QLatin1String("default")
                            : profileId);
    return nullptr;
}

static QJsonObject policyPayload(const QString &profileId,
                                 const char *kind)
{
    return QJsonObject{{QLatin1String("profile"), profileId},
                       {QLatin1String("kind"), QLatin1String(kind)}};
}

// Envelope commands answer {"ok":true,...}; an error frame (or a
// dead transport) surfaces as Undefined.
static bool policyOk(const QJsonValue &r)
{
    return r.isObject() && r[QLatin1String("ok")].toBool();
}

bool get(const char *kind, const QString &key, QString *value)
{
    return get(QString(), kind, key, value);
}

bool set(const char *kind, const QString &key, const QString &value)
{
    return set(QString(), kind, key, value);
}

bool remove(const char *kind, const QString &key)
{
    return remove(QString(), kind, key);
}

bool get(const QString &profileId, const char *kind,
         const QString &key, QString *value)
{
    if (RustCoreBridge *bridge = policyAccess(profileId)) {
        QJsonObject payload = policyPayload(profileId, kind);
        payload.insert(QLatin1String("key"), key);
        const QJsonValue v = bridge->mwQuery(
            AcMw::Policy, AcMw::SitedecGet, payload)
                                 .toObject()
                                 .value(QLatin1String("value"));
        if (!v.isString())
            return false;
        if (value)
            *value = v.toString();
        return true;
    }
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

bool set(const QString &profileId, const char *kind,
         const QString &key, const QString &value)
{
    if (RustCoreBridge *bridge = policyAccess(profileId)) {
        QJsonObject payload = policyPayload(profileId, kind);
        payload.insert(QLatin1String("key"), key);
        payload.insert(QLatin1String("value"), value);
        return policyOk(
            bridge->mwCommand(AcMw::Policy, AcMw::SitedecSet, payload));
    }
    const QByteArray keyUtf8 = key.toUtf8();
    const QByteArray valueUtf8 = value.toUtf8();
    return rc_sitedec_set(kind, keyUtf8.constData(),
                          valueUtf8.constData()) == RC_OK;
}

bool remove(const QString &profileId, const char *kind,
            const QString &key)
{
    if (RustCoreBridge *bridge = policyAccess(profileId)) {
        QJsonObject payload = policyPayload(profileId, kind);
        payload.insert(QLatin1String("key"), key);
        return policyOk(
            bridge->mwCommand(AcMw::Policy, AcMw::SitedecRemove,
                              payload));
    }
    const QByteArray keyUtf8 = key.toUtf8();
    return rc_sitedec_remove(kind, keyUtf8.constData()) == RC_OK;
}

bool clear(const char *kind)
{
    if (RustCoreBridge *bridge = policyAccess(QString())) {
        return policyOk(bridge->mwCommand(
            AcMw::Policy, AcMw::SitedecClear,
            policyPayload(QString(), kind)));
    }
    return rc_sitedec_clear(kind) == RC_OK;
}

static QJsonObject entriesDoc(const QHash<QString, QString> &entries)
{
    QJsonObject obj;
    for (auto it = entries.constBegin(); it != entries.constEnd(); ++it)
        obj.insert(it.key(), it.value());
    return obj;
}

bool replace(const char *kind, const QHash<QString, QString> &entries)
{
    return replace(QString(), kind, entries);
}

bool replace(const QString &profileId, const char *kind,
             const QHash<QString, QString> &entries)
{
    if (RustCoreBridge *bridge = policyAccess(profileId)) {
        QJsonObject payload = policyPayload(profileId, kind);
        payload.insert(QLatin1String("entries"), entriesDoc(entries));
        return policyOk(
            bridge->mwCommand(AcMw::Policy, AcMw::SitedecReplace,
                              payload));
    }
    const QByteArray json =
        QJsonDocument(entriesDoc(entries)).toJson(QJsonDocument::Compact);
    const uint8_t *data = reinterpret_cast<const uint8_t *>(json.constData());
    return rc_sitedec_replace(kind, data, size_t(json.size())) == RC_OK;
}

static QHash<QString, QString> entriesFromDoc(const QJsonObject &obj)
{
    QHash<QString, QString> map;
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
        if (it.value().isString())
            map.insert(it.key(), it.value().toString());
    }
    return map;
}

QHash<QString, QString> entries(const char *kind)
{
    return entries(QString(), kind);
}

QHash<QString, QString> entries(const QString &profileId,
                                const char *kind)
{
    if (RustCoreBridge *bridge = policyAccess(profileId)) {
        return entriesFromDoc(
            bridge
                ->mwQuery(AcMw::Policy, AcMw::SitedecList,
                          policyPayload(profileId, kind))
                .toObject()
                .value(QLatin1String("entries"))
                .toObject());
    }
    QHash<QString, QString> map;
    char *raw = rc_sitedec_list(kind);
    if (!raw)
        return map;
    const QByteArray json = QByteArray::fromRawData(raw, int(strlen(raw)));
    const QJsonObject obj =
        QJsonDocument::fromJson(json).object();
    rc_string_free(raw);
    return entriesFromDoc(obj);
}

QByteArray snapshot()
{
    return snapshot(QString());
}

QByteArray snapshot(const QString &profileId)
{
    if (RustCoreBridge *bridge = policyAccess(profileId)) {
        // The response payload IS the snapshot document.
        const QJsonValue doc = bridge->mwQuery(
            AcMw::Policy, AcMw::SitedecSnapshot,
            QJsonObject{{QLatin1String("profile"), profileId}});
        if (doc.isObject())
            return QJsonDocument(doc.toObject())
                .toJson(QJsonDocument::Compact);
        return QByteArray();
    }
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
    return lookup(QString(), kind, host, value, matchedKey);
}

bool lookup(const QString &profileId, const char *kind,
            const QString &host, QString *value,
            QString *matchedKey)
{
    QJsonObject obj;
    if (RustCoreBridge *bridge = policyAccess(profileId)) {
        QJsonObject payload = policyPayload(profileId, kind);
        payload.insert(QLatin1String("host"), host);
        obj = bridge->mwQuery(AcMw::Policy, AcMw::SitedecLookup, payload)
                  .toObject();
    } else {
        const QByteArray hostUtf8 = host.toUtf8();
        char *raw = rc_sitedec_lookup(kind, hostUtf8.constData());
        if (!raw)
            return false;
        obj = QJsonDocument::fromJson(
            QByteArray::fromRawData(raw, int(strlen(raw)))).object();
        rc_string_free(raw);
    }
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
    if (RustCoreBridge *bridge = policyAccess(QString())) {
        return bridge
            ->mwQuery(AcMw::Policy, AcMw::SitedecStorePresent,
                      QJsonObject{{QLatin1String("profile"), QString()}})
            .toObject()
            .value(QLatin1String("present"))
            .toBool();
    }
    return rc_sitedec_store_present() != 0;
}

bool reload()
{
    if (RustCoreBridge *bridge = policyAccess(QString())) {
        return policyOk(bridge->mwCommand(
            AcMw::Policy, AcMw::SitedecReload,
            QJsonObject{{QLatin1String("profile"), QString()}}));
    }
    return rc_sitedec_reload() == RC_OK;
}

bool reset()
{
    if (RustCoreBridge *bridge = policyAccess(QString())) {
        return policyOk(bridge->mwCommand(
            AcMw::Policy, AcMw::SitedecReset,
            QJsonObject{{QLatin1String("profile"), QString()}}));
    }
    return rc_sitedec_reset() == RC_OK;
}

} // namespace SiteDecisionStore
