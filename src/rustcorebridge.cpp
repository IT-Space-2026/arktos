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

#include <qcoreapplication.h>
#include <qdatetime.h>
#include <qdebug.h>
#include <qfileinfo.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qjsonvalue.h>
#include <qsettings.h>
#include <qthread.h>

#include <atomic>
#include <cstring>

#include <arktos_core.h>

namespace {

// Set once the leaky singleton exists — exists() reads it so a
// non-GUI caller can tell minting is already done (instance() would
// otherwise pin the QObject's affinity to the wrong thread).
bool s_bridgeCreated = false;

// MB06: correlation ids are caller-chosen and echo verbatim — a
// process-wide counter is all the matching the synchronous dispatch
// needs.
std::atomic<quint64> s_mwCorrId{0};

// One framed round trip: build the v1 request (20-byte little-endian
// header + JSON payload), ac_mw_dispatch, unwrap the response.  An
// invalid QJsonValue marks failure; the envelope's error object (or
// a synthesized {"code":"transport"} doc) lands in *errorOut.
QJsonValue mwRoundTrip(AcCore *core, quint8 domain, quint16 op,
                       quint8 msgType, const QJsonObject &payload,
                       QJsonObject *errorOut)
{
    const auto fail = [errorOut](const char *code,
                                 const QString &message) {
        if (errorOut) {
            *errorOut = QJsonObject{
                {QStringLiteral("code"), QString::fromLatin1(code)},
                {QStringLiteral("status"), 0},
                {QStringLiteral("message"), message},
            };
        }
        return QJsonValue(QJsonValue::Undefined);
    };
    if (core == nullptr)
        return fail("transport", QStringLiteral("no AcCore"));

    const QByteArray body =
        QJsonDocument(payload).toJson(QJsonDocument::Compact);
    const quint64 corr = ++s_mwCorrId;
    QByteArray req(20, '\0');
    uchar *h = reinterpret_cast<uchar *>(req.data());
    h[0] = 'A';
    h[1] = 'M';
    h[2] = 1;         // envelope schema v1
    h[3] = msgType;
    for (int i = 0; i < 8; ++i)
        h[4 + i] = uchar(corr >> (8 * i));
    h[12] = domain;
    h[13] = 0;        // flags — reserved
    h[14] = uchar(op & 0xff);
    h[15] = uchar(op >> 8);
    const quint32 bodyLen = quint32(body.size());
    for (int i = 0; i < 4; ++i)
        h[16 + i] = uchar(bodyLen >> (8 * i));
    req.append(body);

    AcBuffer out{nullptr, 0};
    if (ac_mw_dispatch(core,
                       reinterpret_cast<const uint8_t *>(req.constData()),
                       size_t(req.size()), &out) != RC_OK
            || out.data == nullptr) {
        char *err = ac_last_error();
        const QString msg = QString::fromUtf8(err ? err : "dispatch failed");
        ac_string_free(err);
        return fail("transport", msg);
    }
    const QByteArray resp(reinterpret_cast<char *>(out.data),
                          qsizetype(out.len));
    ac_buffer_free(out);

    const uchar *r = reinterpret_cast<const uchar *>(resp.constData());
    if (resp.size() < 20 || r[0] != 'A' || r[1] != 'M' || r[2] != 1)
        return fail("transport", QStringLiteral("malformed response frame"));
    quint64 rcorr = 0;
    for (int i = 0; i < 8; ++i)
        rcorr |= quint64(r[4 + i]) << (8 * i);
    quint32 rlen = 0;
    for (int i = 0; i < 4; ++i)
        rlen |= quint32(r[16 + i]) << (8 * i);
    if (rcorr != corr || r[12] != domain
            || (r[14] | (r[15] << 8)) != op
            || 20 + int(rlen) != resp.size()) {
        return fail("transport",
                    QStringLiteral("response correlation mismatch"));
    }
    const QByteArray rbody = resp.mid(20);
    if (r[3] == 4) {  // error envelope — the structured reply
        if (errorOut)
            *errorOut = QJsonDocument::fromJson(rbody).object();
        return QJsonValue(QJsonValue::Undefined);
    }
    if (r[3] != 3)
        return fail("transport", QStringLiteral("unknown response type"));
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(rbody, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        // A bare `null` payload (an op that had nothing to say).
        if (rbody.trimmed() == "null")
            return QJsonValue(QJsonValue::Null);
        return fail("bad_payload",
                    QStringLiteral("response payload is not JSON"));
    }
    return doc.isArray() ? QJsonValue(doc.array())
                         : QJsonValue(doc.object());
}

} // namespace

QJsonValue RustCoreBridge::mwQuery(int domain, int op,
                                   const QJsonObject &payload,
                                   QJsonObject *errorOut)
{
    return mwRoundTrip(m_core, quint8(domain), quint16(op), 1,
                       payload, errorOut);
}

QJsonValue RustCoreBridge::mwCommand(int domain, int op,
                                     const QJsonObject &payload,
                                     QJsonObject *errorOut)
{
    return mwRoundTrip(m_core, quint8(domain), quint16(op), 2,
                       payload, errorOut);
}

RustCoreBridge *RustCoreBridge::instance()
{
    static RustCoreBridge *self = new RustCoreBridge;
    s_bridgeCreated = true;
    return self;
}

bool RustCoreBridge::exists()
{
    return s_bridgeCreated;
}

RustCoreBridge *RustCoreBridge::serving()
{
    QCoreApplication *app = QCoreApplication::instance();
    if (!app)
        return nullptr;
    if (!exists() && QThread::currentThread() != app->thread())
        return nullptr;
    RustCoreBridge *bridge = instance();
    if (!bridge->core())
        return nullptr;
    // A late root move (the test harness renames the application
    // after BrowserApplication already ran) strands every handle the
    // core minted — they re-assert the OLD root per op.  Refuse so
    // callers take the rc_* lane, which re-anchors per call.
    if (bridge->m_root != BrowserPaths::dataRoot())
        return nullptr;
    return bridge;
}

bool RustCoreBridge::rootDrifted()
{
    if (!exists())
        return false;
    return instance()->m_root != BrowserPaths::dataRoot();
}

RustCoreBridge::RustCoreBridge(QObject *parent)
    : QObject(parent)
{
    rc_set_change_callback(&RustCoreBridge::trampoline, this);

    // RC07: the app-lifetime facade root.  Bound once to the app
    // data root — the accessors re-assert profile bindings per call,
    // so the handle stays correct across profile switches without
    // the rustCoreEnsureDataDirFor dance.  m_root records the binding
    // so serving() can tell a late root move (test-mode renames)
    // from a stable one.
    m_root = BrowserPaths::dataRoot();
    const QByteArray root = m_root.toUtf8();
    // Same default-name seeding as RustCoreProfiles: the PROF01-era
    // QSettings key first, the localized fallback after.
    QByteArray seed = QSettings()
        .value(QLatin1String("profile/name")).toString().toUtf8();
    if (seed.isEmpty())
        seed = QCoreApplication::translate("ProfileRegistry", "Profile")
            .toUtf8();
    AcConfig cfg;
    cfg.struct_size = sizeof(cfg);
    cfg.api_version = AC_API_VERSION;
    cfg.data_root = root.constData();
    cfg.app_name = "arktos";
    cfg.default_profile_name = seed.constData();
    m_core = ac_init(&cfg);
    if (m_core != nullptr) {
        ac_set_change_callback(m_core, &RustCoreBridge::coreTrampoline,
                               this);
    } else {
        char *err = ac_last_error();
        qWarning() << "RustCoreBridge: ac_init failed:"
                   << QString::fromUtf8(err ? err : "unknown");
        ac_string_free(err);
    }
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

void RustCoreBridge::coreTrampoline(const char *module,
                                    const char *profileId,
                                    void *userdata)
{
    RustCoreBridge *self = static_cast<RustCoreBridge *>(userdata);
    const QString t = QString::fromUtf8(module ? module : "");
    const QString p = QString::fromUtf8(profileId ? profileId : "");
    // Same any-thread hop as trampoline; only the scoped signal
    // fires — storeChanged stays the legacy callback's alone so
    // listeners don't see double traffic.
    QMetaObject::invokeMethod(self, [self, t, p]() {
        emit self->storeChangedFor(t, p);
    }, Qt::QueuedConnection);
}

RcSettings *RustCoreBridge::settings(const QString &profileId)
{
    if (m_core == nullptr)
        return nullptr;
    const QByteArray id = profileId.toUtf8();
    return ac_settings(m_core,
                       id.isEmpty() ? nullptr : id.constData());
}

RcProfiles *RustCoreBridge::profiles()
{
    if (m_core == nullptr)
        return nullptr;
    return ac_profiles(m_core);
}

AcHistory *RustCoreBridge::history(const QString &profileId)
{
    if (m_core == nullptr)
        return nullptr;
    const QByteArray id = profileId.toUtf8();
    return ac_history(m_core,
                      id.isEmpty() ? nullptr : id.constData());
}

AcBookmarks *RustCoreBridge::bookmarks(const QString &profileId)
{
    if (m_core == nullptr)
        return nullptr;
    const QByteArray id = profileId.toUtf8();
    return ac_bookmarks(m_core,
                        id.isEmpty() ? nullptr : id.constData());
}

AcPolicy *RustCoreBridge::policy(const QString &profileId)
{
    if (m_core == nullptr)
        return nullptr;
    const QByteArray id = profileId.toUtf8();
    return ac_policy(m_core,
                     id.isEmpty() ? nullptr : id.constData());
}

AcSession *RustCoreBridge::session(const QString &profileId)
{
    if (m_core == nullptr)
        return nullptr;
    const QByteArray id = profileId.toUtf8();
    return ac_session(m_core,
                      id.isEmpty() ? nullptr : id.constData());
}

AcIcons *RustCoreBridge::icons(const QString &profileId)
{
    if (m_core == nullptr)
        return nullptr;
    const QByteArray id = profileId.toUtf8();
    return ac_icons(m_core,
                    id.isEmpty() ? nullptr : id.constData());
}

RcNotes *RustCoreBridge::notes(const QString &profileId)
{
    if (m_core == nullptr)
        return nullptr;
    const QByteArray id = profileId.toUtf8();
    return ac_notes(m_core,
                    id.isEmpty() ? nullptr : id.constData());
}

AcDlwpolicy *RustCoreBridge::dlwpolicy(const QString &profileId)
{
    if (m_core == nullptr)
        return nullptr;
    const QByteArray id = profileId.toUtf8();
    return ac_dlwpolicy(m_core,
                        id.isEmpty() ? nullptr : id.constData());
}

AcPrivacy *RustCoreBridge::privacy(const QString &profileId)
{
    if (m_core == nullptr)
        return nullptr;
    const QByteArray id = profileId.toUtf8();
    return ac_privacy(m_core,
                      id.isEmpty() ? nullptr : id.constData());
}

AcUrlclean *RustCoreBridge::urlclean(const QString &profileId)
{
    if (m_core == nullptr)
        return nullptr;
    const QByteArray id = profileId.toUtf8();
    return ac_urlclean(m_core,
                       id.isEmpty() ? nullptr : id.constData());
}

AcCookies *RustCoreBridge::cookies(const QString &profileId)
{
    if (m_core == nullptr)
        return nullptr;
    const QByteArray id = profileId.toUtf8();
    return ac_cookies(m_core,
                      id.isEmpty() ? nullptr : id.constData());
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

QJsonObject rustCoreSessionPolicyCall(const QString &profileId,
                                      const QJsonObject &request)
{
    AcSession *h = nullptr;
    if (RustCoreBridge *bridge = RustCoreBridge::serving())
        h = bridge->session(profileId);
    RcSettings *s = nullptr;
    if (!h) {
        const QByteArray root = BrowserPaths::dataRoot().toUtf8();
        const QByteArray id = profileId.isEmpty()
            ? QByteArrayLiteral("default") : profileId.toUtf8();
        s = rc_settings_open(root.constData(), id.constData());
        if (!s)
            return QJsonObject();
    }
    const QByteArray json =
        QJsonDocument(request).toJson(QJsonDocument::Compact);
    char *raw = h
        ? ac_session_policy(h,
                            reinterpret_cast<const uint8_t *>(json.constData()),
                            size_t(json.size()))
        : rc_sesspolicy_call(s,
                             reinterpret_cast<const uint8_t *>(json.constData()),
                             size_t(json.size()));
    ac_session_close(h);
    if (s)
        rc_settings_close(s);
    if (!raw)
        return QJsonObject();
    QByteArray out = QByteArray::fromRawData(raw, int(strlen(raw)));
    out.detach();
    rc_string_free(raw);
    return QJsonDocument::fromJson(out).object();
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

// QVariant -> the store's typed {"t","v"} document — the same shape
// rc_settings_get hands back, so value() decodes both lanes with one
// parser.  Mirrors setValue's flat type switch.
QJsonObject settingsDocFrom(const QVariant &value)
{
    const QString t = QStringLiteral("t");
    const QString v = QStringLiteral("v");
    switch (value.typeId()) {
    case QMetaType::Bool:
        return {{t, QStringLiteral("b")}, {v, value.toBool()}};
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
        return {{t, QStringLiteral("i")}, {v, value.toLongLong()}};
    case QMetaType::Double:
    case QMetaType::Float:
        return {{t, QStringLiteral("d")}, {v, value.toDouble()}};
    case QMetaType::QByteArray:
        return {{t, QStringLiteral("x")},
                {v, QString::fromLatin1(value.toByteArray().toHex())}};
    case QMetaType::QStringList:
        return {{t, QStringLiteral("l")},
                {v, QJsonArray::fromStringList(value.toStringList())}};
    default:
        return {{t, QStringLiteral("s")}, {v, value.toString()}};
    }
}

} // namespace

RustCoreSettings::RustCoreSettings(const QString &profileId)
    : m_h(nullptr)
    , m_profileId(profileId)
{
    // MB06: no eager mint — the envelope lane binds the scope per
    // call through the shared AcCore, so the only handle left to
    // want is the lazy rc_settings_open fallback below.
}

RustCoreSettings::~RustCoreSettings()
{
    // ac_settings_close is NULL-tolerant and also legal on an
    // rc_settings_open handle (its core-untrack lookup just misses).
    ac_settings_close(m_h);
}

RcSettings *RustCoreSettings::fallbackStore() const
{
    // Reached only when the bridge cannot serve — the legacy
    // spelling over a per-object open, minted on first use.  A
    // minted handle pins its root, so a data-root move remints.
    if (m_h != nullptr && RustCoreBridge::rootDrifted()) {
        ac_settings_close(m_h);
        m_h = nullptr;
    }
    if (m_h == nullptr) {
        const QByteArray root = BrowserPaths::dataRoot().toUtf8();
        const QByteArray id = m_profileId.toUtf8();
        m_h = rc_settings_open(root.constData(),
                               id.isEmpty() ? nullptr : id.constData());
    }
    return m_h;
}

bool RustCoreSettings::isValid() const
{
    return RustCoreBridge::serving() != nullptr
        || fallbackStore() != nullptr;
}

QString RustCoreSettings::profileId() const
{
    return m_profileId;
}

QVariant RustCoreSettings::value(const QString &key,
                                 const QVariant &defaultValue) const
{
    QJsonObject row;
    if (RustCoreBridge *bridge = RustCoreBridge::serving()) {
        // settings.get answers the same {"t","v"} doc the flat
        // spelling returned — a missing key is an error frame,
        // which reads as the default either way.
        const QJsonValue v = bridge->mwQuery(
            AcMw::Settings, AcMw::SettingsGet,
            QJsonObject{{QStringLiteral("key"), key},
                        {QStringLiteral("profile"), m_profileId}});
        if (v.isUndefined())
            return defaultValue;
        row = v.toObject();
    } else {
        RcSettings *h = fallbackStore();
        if (h == nullptr)
            return defaultValue;
        RcBuffer buf{nullptr, 0};
        if (rc_settings_get(h, key.toUtf8().constData(), &buf) != RC_OK)
            return defaultValue;
        row = QJsonDocument::fromJson(takeBuffer(buf)).object();
    }
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
    if (RustCoreBridge *bridge = RustCoreBridge::serving()) {
        // One typed doc through settings.set — the six flat
        // rc_settings_set_<type> exports collapse into this op.
        bridge->mwCommand(AcMw::Settings, AcMw::SettingsSet,
                          QJsonObject{{QStringLiteral("key"), key},
                                      {QStringLiteral("profile"), m_profileId},
                                      {QStringLiteral("value"),
                                       settingsDocFrom(value)}});
        return;
    }
    RcSettings *h = fallbackStore();
    if (h == nullptr)
        return;
    const QByteArray k = key.toUtf8();
    switch (value.typeId()) {
    case QMetaType::Bool:
        rc_settings_set_bool(h, k.constData(), value.toBool() ? 1 : 0);
        break;
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
        rc_settings_set_int(h, k.constData(), value.toLongLong());
        break;
    case QMetaType::Double:
    case QMetaType::Float:
        rc_settings_set_double(h, k.constData(), value.toDouble());
        break;
    case QMetaType::QByteArray: {
        const QByteArray bytes = value.toByteArray();
        rc_settings_set_bytes(h, k.constData(),
                              reinterpret_cast<const uint8_t *>(
                                  bytes.constData()),
                              static_cast<size_t>(bytes.size()));
        break;
    }
    case QMetaType::QStringList: {
        const QByteArray json = QJsonDocument(
            QJsonArray::fromStringList(value.toStringList())).toJson();
        rc_settings_set_list(h, k.constData(),
                             reinterpret_cast<const uint8_t *>(
                                 json.constData()),
                             static_cast<size_t>(json.size()));
        break;
    }
    default: {
        const QByteArray s = value.toString().toUtf8();
        rc_settings_set_string(h, k.constData(), s.constData());
        break;
    }
    }
}

void RustCoreSettings::remove(const QString &key)
{
    if (RustCoreBridge *bridge = RustCoreBridge::serving()) {
        bridge->mwCommand(AcMw::Settings, AcMw::SettingsRemove,
                          QJsonObject{{QStringLiteral("key"), key},
                                      {QStringLiteral("profile"), m_profileId}});
        return;
    }
    RcSettings *h = fallbackStore();
    if (h == nullptr)
        return;
    rc_settings_remove(h, key.toUtf8().constData());
}

bool RustCoreSettings::contains(const QString &key) const
{
    if (RustCoreBridge *bridge = RustCoreBridge::serving()) {
        return bridge->mwQuery(
                   AcMw::Settings, AcMw::SettingsContains,
                   QJsonObject{{QStringLiteral("key"), key},
                               {QStringLiteral("profile"), m_profileId}})
            .toObject()
            .value(QLatin1String("present"))
            .toBool();
    }
    RcSettings *h = fallbackStore();
    if (h == nullptr)
        return false;
    return rc_settings_contains(h, key.toUtf8().constData()) != 0;
}

QStringList RustCoreSettings::allKeys(const QString &prefix) const
{
    QStringList out;
    if (RustCoreBridge *bridge = RustCoreBridge::serving()) {
        // settings.list — the batch read the envelope exists for.
        const QJsonArray keys = bridge->mwQuery(
                                    AcMw::Settings, AcMw::SettingsList,
                                    QJsonObject{
                                        {QStringLiteral("prefix"), prefix},
                                        {QStringLiteral("profile"),
                                         m_profileId}})
                                    .toObject()
                                    .value(QLatin1String("keys"))
                                    .toArray();
        out.reserve(keys.size());
        for (const QJsonValue &k : keys)
            out.append(k.toString());
        return out;
    }
    RcSettings *h = fallbackStore();
    if (h == nullptr)
        return out;
    char *json = rc_settings_list(
        h, prefix.isEmpty() ? nullptr : prefix.toUtf8().constData());
    if (json == nullptr)
        return out;
    const QByteArray text = QByteArray(json);
    rc_string_free(json);
    const QJsonArray keys = QJsonDocument::fromJson(text).array();
    out.reserve(keys.size());
    for (const QJsonValue &k : keys)
        out.append(k.toString());
    return out;
}

void RustCoreSettings::clear()
{
    // The store has no bulk clear; removing each listed key keeps the
    // same remove-and-persist lane one key at a time (either leg).
    const QStringList keys = allKeys();
    for (const QString &key : keys)
        remove(key);
}

void RustCoreSettings::sync()
{
    if (RustCoreBridge *bridge = RustCoreBridge::serving()) {
        bridge->mwCommand(AcMw::Settings, AcMw::SettingsSync,
                          QJsonObject{{QStringLiteral("profile"),
                                       m_profileId}});
        return;
    }
    RcSettings *h = fallbackStore();
    if (h == nullptr)
        return;
    rc_settings_sync(h);
}

bool RustCoreSettings::importLegacyIni(const QString &iniPath)
{
    if (RustCoreBridge *bridge = RustCoreBridge::serving()) {
        return !bridge->mwCommand(
                           AcMw::Settings, AcMw::SettingsImportIni,
                           QJsonObject{{QStringLiteral("path"), iniPath},
                                       {QStringLiteral("profile"),
                                        m_profileId}})
                    .isUndefined();
    }
    RcSettings *h = fallbackStore();
    if (h == nullptr)
        return false;
    return rc_settings_import_ini(h, iniPath.toUtf8().constData())
        == RC_OK;
}

// ---- RC03: RustCoreProfiles ---------------------------------------

namespace {

QString takeString(char *p)
{
    if (p == nullptr)
        return QString();
    const QString out = QString::fromUtf8(p);
    rc_string_free(p);
    return out;
}

ProfileContext contextFromJson(const QJsonObject &row)
{
    ProfileContext context;
    context.id = row.value(QLatin1String("id")).toString();
    context.name = row.value(QLatin1String("name")).toString();
    context.avatar = row.value(QLatin1String("avatar")).toString();
    context.createdAt = QDateTime::fromString(
        row.value(QLatin1String("createdAt")).toString(), Qt::ISODate);
    return context;
}

} // namespace

RustCoreProfiles::RustCoreProfiles(const QString &dataRoot)
    : m_h(nullptr)
    , m_root(dataRoot)
{
    // MB06: the registry ops run through the envelope when this is
    // the app root and the bridge serves — per-call binding needs no
    // handle.  An explicit root is a fixture scope the shared AcCore
    // never saw, so it keeps the lazy rc_profiles_open fallback.
}

RustCoreProfiles::~RustCoreProfiles()
{
    // ac_profiles_close accepts rc_profiles_open handles too.
    ac_profiles_close(m_h);
}

RcProfiles *RustCoreProfiles::fallbackStore() const
{
    // A minted handle pins its root — remint on drift so a test-mode
    // move can never strand it on the old root (only for the shared
    // dataRoot spelling; an explicit fixture root can't drift).
    if (m_h != nullptr && m_root.isEmpty()
            && RustCoreBridge::rootDrifted()) {
        ac_profiles_close(m_h);
        m_h = nullptr;
    }
    if (m_h == nullptr) {
        const QString resolved =
            m_root.isEmpty() ? BrowserPaths::dataRoot() : m_root;
        // The synthesized default row's name seeds exactly like
        // ProfileRegistry::load: the PROF01-era QSettings key first,
        // the localized fallback after.
        QByteArray seed = QSettings()
            .value(QLatin1String("profile/name")).toString().toUtf8();
        if (seed.isEmpty())
            seed = QCoreApplication::translate("ProfileRegistry",
                                               "Profile").toUtf8();
        const QByteArray root = resolved.toUtf8();
        m_h = rc_profiles_open(root.constData(), seed.constData());
    }
    return m_h;
}

bool RustCoreProfiles::servingEnvelope() const
{
    return m_root.isEmpty() && RustCoreBridge::serving() != nullptr;
}

bool RustCoreProfiles::isValid() const
{
    return servingEnvelope() || fallbackStore() != nullptr;
}

QString RustCoreProfiles::filePath() const
{
    // A path derivation, not a store read — stays flat.
    if (fallbackStore() == nullptr)
        return QString();
    return takeString(rc_profiles_file(m_h));
}

QList<ProfileContext> RustCoreProfiles::profiles() const
{
    QList<ProfileContext> out;
    if (servingEnvelope()) {
        const QJsonArray rows = RustCoreBridge::serving()
            ->mwQuery(AcMw::Profiles, AcMw::ProfilesList)
            .toObject()
            .value(QLatin1String("profiles"))
            .toArray();
        out.reserve(rows.size());
        for (const QJsonValue &row : rows)
            out.append(contextFromJson(row.toObject()));
        return out;
    }
    if (fallbackStore() == nullptr)
        return out;
    char *json = rc_profiles_list(m_h);
    if (json == nullptr)
        return out;
    const QByteArray text = QByteArray(json);
    rc_string_free(json);
    const QJsonArray rows = QJsonDocument::fromJson(text).array();
    out.reserve(rows.size());
    for (const QJsonValue &row : rows)
        out.append(contextFromJson(row.toObject()));
    return out;
}

ProfileContext RustCoreProfiles::profile(const QString &id) const
{
    if (servingEnvelope()) {
        const QJsonValue row = RustCoreBridge::serving()->mwQuery(
            AcMw::Profiles, AcMw::ProfilesGet,
            QJsonObject{{QStringLiteral("id"), id}});
        if (row.isUndefined())
            return ProfileContext();
        return contextFromJson(row.toObject());
    }
    if (fallbackStore() == nullptr)
        return ProfileContext();
    char *json = rc_profiles_get(m_h, id.toUtf8().constData());
    if (json == nullptr)
        return ProfileContext();
    const QByteArray text = QByteArray(json);
    rc_string_free(json);
    return contextFromJson(
        QJsonDocument::fromJson(text).object());
}

QString RustCoreProfiles::activeProfileId() const
{
    // FLAT by contract: the ambient persona is per-handle process
    // state — an envelope op would read a fresh handle's "default"
    // and set_active would land on a handle that dies with the call.
    if (fallbackStore() == nullptr)
        return QStringLiteral("default");
    return takeString(rc_profiles_active(m_h));
}

void RustCoreProfiles::setActiveProfileId(const QString &id)
{
    // Same per-handle rule as activeProfileId — never the envelope.
    if (fallbackStore() == nullptr)
        return;
    rc_profiles_set_active(m_h, id.toUtf8().constData());
}

ProfileContext RustCoreProfiles::createProfile(const QString &displayName)
{
    QString id;
    if (servingEnvelope()) {
        id = RustCoreBridge::serving()
            ->mwCommand(AcMw::Profiles, AcMw::ProfilesCreate,
                        QJsonObject{{QStringLiteral("display_name"),
                                     displayName}})
            .toObject()
            .value(QLatin1String("id"))
            .toString();
    } else if (fallbackStore() != nullptr) {
        id = takeString(
            rc_profiles_create(m_h, displayName.toUtf8().constData()));
    }
    if (id.isEmpty())
        return ProfileContext();
    return profile(id);
}

bool RustCoreProfiles::renameProfile(const QString &id,
                                     const QString &name)
{
    if (servingEnvelope()) {
        return !RustCoreBridge::serving()
            ->mwCommand(AcMw::Profiles, AcMw::ProfilesRename,
                        QJsonObject{{QStringLiteral("id"), id},
                                    {QStringLiteral("name"), name}})
            .isUndefined();
    }
    if (fallbackStore() == nullptr)
        return false;
    return rc_profiles_rename(m_h, id.toUtf8().constData(),
                              name.toUtf8().constData()) == RC_OK;
}

bool RustCoreProfiles::setAvatarName(const QString &id,
                                     const QString &fileName)
{
    if (servingEnvelope()) {
        return !RustCoreBridge::serving()
            ->mwCommand(AcMw::Profiles, AcMw::ProfilesSetAvatar,
                        QJsonObject{{QStringLiteral("id"), id},
                                    {QStringLiteral("name"),
                                     fileName.isEmpty()
                                         ? QJsonValue(QJsonValue::Null)
                                         : QJsonValue(fileName)}})
            .isUndefined();
    }
    if (fallbackStore() == nullptr)
        return false;
    const QByteArray name = fileName.toUtf8();
    return rc_profiles_set_avatar(
               m_h, id.toUtf8().constData(),
               name.isEmpty() ? nullptr : name.constData())
        == RC_OK;
}

bool RustCoreProfiles::removeAvatar(const QString &id)
{
    return setAvatarName(id, QString());
}

bool RustCoreProfiles::deleteProfile(const QString &id)
{
    if (servingEnvelope()) {
        return !RustCoreBridge::serving()
            ->mwCommand(AcMw::Profiles, AcMw::ProfilesDelete,
                        QJsonObject{{QStringLiteral("id"), id}})
            .isUndefined();
    }
    if (fallbackStore() == nullptr)
        return false;
    return rc_profiles_delete(m_h, id.toUtf8().constData()) == RC_OK;
}

bool RustCoreProfiles::reload()
{
    if (servingEnvelope()) {
        return !RustCoreBridge::serving()
            ->mwCommand(AcMw::Profiles, AcMw::ProfilesReload)
            .isUndefined();
    }
    if (fallbackStore() == nullptr)
        return false;
    return rc_profiles_reload(m_h) == RC_OK;
}

namespace {

using PathFn = char *(*)(RcProfiles *, const char *);

QString pathVia(RcProfiles *h, PathFn fn, const QString &id)
{
    if (h == nullptr)
        return QString();
    const QByteArray utf8 = id.toUtf8();
    return takeString(fn(h, utf8.constData()));
}

} // namespace

QString RustCoreProfiles::profileDir(const QString &id) const
{
    // Path derivations stay flat — no store I/O to batch.
    return pathVia(fallbackStore(), &rc_profiles_dir, id);
}

QString RustCoreProfiles::profileCacheDir(const QString &id) const
{
    return pathVia(fallbackStore(), &rc_profiles_cache_dir, id);
}

QString RustCoreProfiles::engineStorageDir(const QString &id) const
{
    return pathVia(fallbackStore(), &rc_profiles_engine_storage_dir, id);
}

QString RustCoreProfiles::engineCacheDir(const QString &id) const
{
    return pathVia(fallbackStore(), &rc_profiles_engine_cache_dir, id);
}

QString RustCoreProfiles::settingsDir(const QString &id) const
{
    return pathVia(fallbackStore(), &rc_profiles_settings_dir, id);
}

QString RustCoreProfiles::slugify(const QString &displayName)
{
    return takeString(
        rc_profiles_slugify(displayName.toUtf8().constData()));
}
