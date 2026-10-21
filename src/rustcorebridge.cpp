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

#include "rustcorebridge.h"

#include "browserpaths.h"

#include <qfileinfo.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qjsonvalue.h>

#include <rustcore.h>

RustCoreBridge *RustCoreBridge::instance()
{
    static RustCoreBridge *self = new RustCoreBridge;
    return self;
}

RustCoreBridge::RustCoreBridge(QObject *parent)
    : QObject(parent)
{
    rc_set_change_callback(&RustCoreBridge::trampoline, this);
}

void RustCoreBridge::trampoline(const char *topic, void *userdata)
{
    RustCoreBridge *self = static_cast<RustCoreBridge *>(userdata);
    const QString t = QString::fromUtf8(topic ? topic : "");
    // Rust may call from any thread; hop to the object's thread.
    QMetaObject::invokeMethod(self, [self, t]() {
        emit self->storeChanged(t);
    }, Qt::QueuedConnection);
}

void rustCoreEnsureDataDir()
{
    // Cheap and idempotent — call per operation so test-mode
    // data-dir switches propagate.
    const QByteArray dir =
        QFileInfo(BrowserPaths::dataFilePath(QLatin1String(".rc")))
            .absolutePath().toUtf8();
    rc_set_data_dir(dir.constData());
}

void rustCoreEnsureDataDirFor(const QString &profileId)
{
    const QByteArray dir =
        BrowserPaths::profileDataDir(profileId).toUtf8();
    rc_set_data_dir(dir.constData());
}

// ---- RC02: RustCoreSettings ---------------------------------------

namespace {

QByteArray takeBuffer(RcBuffer &buf)
{
    if (buf.data == nullptr)
        return QByteArray();
    const QByteArray out(reinterpret_cast<char *>(buf.data),
                         static_cast<qsizetype>(buf.len));
    rc_buffer_free(buf);
    return out;
}

} // namespace

RustCoreSettings::RustCoreSettings(const QString &profileId)
    : m_h(nullptr)
    , m_profileId(profileId)
{
    const QByteArray root = BrowserPaths::dataRoot().toUtf8();
    const QByteArray id = profileId.toUtf8();
    m_h = rc_settings_open(root.constData(),
                           id.isEmpty() ? nullptr : id.constData());
}

RustCoreSettings::~RustCoreSettings()
{
    rc_settings_close(m_h);
}

bool RustCoreSettings::isValid() const
{
    return m_h != nullptr;
}

QString RustCoreSettings::profileId() const
{
    return m_profileId;
}

QVariant RustCoreSettings::value(const QString &key,
                                 const QVariant &defaultValue) const
{
    if (m_h == nullptr)
        return defaultValue;
    RcBuffer buf{nullptr, 0};
    if (rc_settings_get(m_h, key.toUtf8().constData(), &buf) != RC_OK)
        return defaultValue;
    const QJsonObject row =
        QJsonDocument::fromJson(takeBuffer(buf)).object();
    const QString t = row.value(QLatin1String("t")).toString();
    const QJsonValue v = row.value(QLatin1String("v"));
    if (t == QLatin1String("s"))
        return v.toString();
    if (t == QLatin1String("b"))
        return v.toBool();
    if (t == QLatin1String("i"))
        return QVariant::fromValue(v.toInteger());
    if (t == QLatin1String("d"))
        return v.toDouble();
    if (t == QLatin1String("x"))
        return QByteArray::fromHex(v.toString().toLatin1());
    if (t == QLatin1String("l")) {
        QStringList list;
        const QJsonArray items = v.toArray();
        list.reserve(items.size());
        for (const QJsonValue &item : items)
            list.append(item.toString());
        return list;
    }
    return defaultValue;
}

void RustCoreSettings::setValue(const QString &key,
                                const QVariant &value)
{
    if (m_h == nullptr)
        return;
    const QByteArray k = key.toUtf8();
    switch (value.typeId()) {
    case QMetaType::Bool:
        rc_settings_set_bool(m_h, k.constData(), value.toBool() ? 1 : 0);
        break;
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
        rc_settings_set_int(m_h, k.constData(), value.toLongLong());
        break;
    case QMetaType::Double:
    case QMetaType::Float:
        rc_settings_set_double(m_h, k.constData(), value.toDouble());
        break;
    case QMetaType::QByteArray: {
        const QByteArray bytes = value.toByteArray();
        rc_settings_set_bytes(m_h, k.constData(),
                              reinterpret_cast<const uint8_t *>(
                                  bytes.constData()),
                              static_cast<size_t>(bytes.size()));
        break;
    }
    case QMetaType::QStringList: {
        const QByteArray json = QJsonDocument(
            QJsonArray::fromStringList(value.toStringList())).toJson();
        rc_settings_set_list(m_h, k.constData(),
                             reinterpret_cast<const uint8_t *>(
                                 json.constData()),
                             static_cast<size_t>(json.size()));
        break;
    }
    default: {
        const QByteArray s = value.toString().toUtf8();
        rc_settings_set_string(m_h, k.constData(), s.constData());
        break;
    }
    }
}

void RustCoreSettings::remove(const QString &key)
{
    if (m_h == nullptr)
        return;
    rc_settings_remove(m_h, key.toUtf8().constData());
}

bool RustCoreSettings::contains(const QString &key) const
{
    if (m_h == nullptr)
        return false;
    return rc_settings_contains(m_h, key.toUtf8().constData()) != 0;
}

QStringList RustCoreSettings::allKeys(const QString &prefix) const
{
    if (m_h == nullptr)
        return {};
    char *json = rc_settings_list(
        m_h, prefix.isEmpty() ? nullptr : prefix.toUtf8().constData());
    if (json == nullptr)
        return {};
    const QByteArray text = QByteArray(json);
    rc_string_free(json);
    const QJsonArray keys = QJsonDocument::fromJson(text).array();
    QStringList out;
    out.reserve(keys.size());
    for (const QJsonValue &k : keys)
        out.append(k.toString());
    return out;
}

void RustCoreSettings::sync()
{
    if (m_h == nullptr)
        return;
    rc_settings_sync(m_h);
}

bool RustCoreSettings::importLegacyIni(const QString &iniPath)
{
    if (m_h == nullptr)
        return false;
    return rc_settings_import_ini(m_h, iniPath.toUtf8().constData())
        == RC_OK;
}
